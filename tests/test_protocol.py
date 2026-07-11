import io
import os
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import trans_type as tool


ALLOWED_COMMAND_CHARS = set("abcdefghijklmnopqrstuvwxyz0123456789 \r\n-./\\'")


def options(*args: str) -> tool.Options:
    return tool.parse_args(list(args))


def text_data(text: str, raw: bytes | None = None) -> tool.TextData:
    encoded = text.encode("utf-8") if raw is None else raw
    return tool.TextData(Path("fixture.txt"), text, len(encoded), "test", raw)


def assert_safe_stream(test: unittest.TestCase, commands: str) -> None:
    tool.validate_generated_command_stream(commands)
    test.assertTrue(set(commands) <= ALLOWED_COMMAND_CHARS)
    test.assertFalse(any("A" <= ch <= "Z" for ch in commands))


def execute_command_stream(commands: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    lines = commands.splitlines()
    if not lines or lines[0] != "powershell -noprofile" or lines[-1] != "exit":
        raise AssertionError("unexpected generated command framing")
    script = "\n".join(lines[1:-1]) + "\n"
    return subprocess.run(
        ["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", "-"],
        input=script,
        text=True,
        cwd=cwd,
        capture_output=True,
        check=True,
    )


class ProtocolTests(unittest.TestCase):
    def test_space_pause_erases_control_keys_and_resumes(self) -> None:
        class FakeKeyboard:
            def __init__(self) -> None:
                self.space_states = iter((0, 0x8000, 0))
                self.backspaces = 0

            def get_async_key_state(self, key: int) -> int:
                if key == tool.VK_SPACE:
                    return next(self.space_states, 0)
                return 0

            def foreground_window(self) -> tool.TargetWindow:
                return tool.TargetWindow(1, 2, "target")

            def legacy_vk(self, key: int) -> None:
                if key == tool.VK_BACK:
                    self.backspaces += 1

        keyboard = FakeKeyboard()
        rc, target = tool.pause_until_space(
            keyboard, options("--mode", "simple"), tool.TargetWindow(1, 2, "target")
        )
        self.assertEqual(rc, tool.EXIT_OK)
        self.assertEqual(target.hwnd, 1)
        self.assertEqual(keyboard.backspaces, 2)

    def test_numeric_hex_payload_is_single_quoted(self) -> None:
        opt = options("--mode", "cmd-hex")
        payload, _, _ = tool.hex_payload("\t123", opt.hex_chunk_bytes)
        commands = tool.build_cmd_hex_commands(payload, opt)
        self.assertIn("'09313233'", commands)
        self.assertIn("'tt.hex'", commands)
        assert_safe_stream(self, commands)

    def test_complex_samples_never_expose_source_symbols(self) -> None:
        samples = ["plain text\n", "@#$% quotes ' \"\n", "中文与emoji✓\n", "1234567890" * 100]
        for mode in ("cmd-hex", "zip-hex"):
            opt = options("--mode", mode)
            for sample in samples:
                raw = sample.encode("utf-8")
                transfer = tool.zip_payload(raw, opt) if mode == "zip-hex" else raw
                payload, _, _ = tool.hex_payload_from_bytes(transfer, opt.hex_chunk_bytes)
                commands = (
                    tool.build_zip_hex_commands(payload, opt)
                    if mode == "zip-hex"
                    else tool.build_cmd_hex_commands(payload, opt)
                )
                assert_safe_stream(self, commands)
                self.assertIn("certutil -hashfile '.\\trans.txt' sha256", commands)
                self.assertIn("remove-item -force 'tt.hex'", commands)
                if mode == "zip-hex":
                    self.assertIn("remove-item -force 'tt.zip'", commands)

    def test_output_encoding_modes(self) -> None:
        data = text_data("中文", b"original-bytes")
        self.assertEqual(tool.output_bytes(data, options("--mode", "cmd-hex")), "中文".encode())
        self.assertEqual(
            tool.output_bytes(data, options("--mode", "cmd-hex", "--output-encoding", "utf8-bom")),
            b"\xef\xbb\xbf" + "中文".encode(),
        )
        self.assertEqual(
            tool.output_bytes(data, options("--mode", "cmd-hex", "--output-encoding", "preserve")),
            b"original-bytes",
        )

    def test_zip_payload_compresses_and_roundtrips(self) -> None:
        raw = b"repetitive line 0123456789\n" * 2000
        zipped = tool.zip_payload(raw, options("--mode", "zip-hex"))
        self.assertLess(len(zipped), len(raw) // 10)
        with zipfile.ZipFile(io.BytesIO(zipped), "r") as archive:
            self.assertEqual(archive.namelist(), ["trans.txt"])
            self.assertEqual(archive.read("trans.txt"), raw)

    def test_source_path_sets_output_basename_and_file_option_is_removed(self) -> None:
        direct = options("--mode", "cmd-hex", "--source", "some/path.bin", "--output-encoding", "preserve")
        self.assertEqual(direct.source, "file")
        self.assertEqual(direct.file_path, Path("some/path.bin"))
        self.assertEqual(direct.remote_output, ".\\path.bin")
        self.assertEqual(options("--mode", "cmd-hex", "--source", "clipboard").remote_output, ".\\trans.txt")
        for removed in ("--file", "--remote-hex", "--remote-zip", "--remote-path"):
            with self.assertRaises(SystemExit, msg=removed):
                options(removed, "unused")

    def test_simple_mode_rejects_remote_output_options(self) -> None:
        for arguments in (
            ("--remote-output", "trans.txt"),
            ("--commands-out", "commands.txt"),
        ):
            with self.assertRaises(SystemExit, msg=arguments):
                options("--mode", "simple", *arguments)

    def test_preserve_is_restricted_to_file_complex_modes(self) -> None:
        with self.assertRaises(SystemExit):
            options("--mode", "simple", "--output-encoding", "preserve")
        with self.assertRaises(SystemExit):
            options("--mode", "cmd-hex", "--source", "clipboard", "--output-encoding", "preserve")

    def test_arbitrary_binary_file_roundtrips_in_complex_modes(self) -> None:
        raw = bytes(range(256)) * 4
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "payload.bin"
            source.write_bytes(raw)
            for mode in ("cmd-hex", "zip-hex"):
                opt = options(
                    "--mode",
                    mode,
                    "--source",
                    str(source),
                    "--output-encoding",
                    "preserve",
                    "--remote-output",
                    "payload.bin",
                )
                data = tool.read_text_source(opt)
                self.assertEqual(data.source_kind, "file")
                self.assertEqual(tool.output_bytes(data, opt), raw)
                tool.validate_text(data, tool.analyze_text(data.text), opt)
                transfer = tool.zip_payload(raw, opt) if mode == "zip-hex" else raw
                payload, _, _ = tool.hex_payload_from_bytes(transfer, opt.hex_chunk_bytes)
                commands = (
                    tool.build_zip_hex_commands(payload, opt)
                    if mode == "zip-hex"
                    else tool.build_cmd_hex_commands(payload, opt)
                )
                assert_safe_stream(self, commands)

    def test_directory_source_builds_content_only_zip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            (source / "Nested").mkdir(parents=True)
            (source / "empty").mkdir()
            (source / "root.bin").write_bytes(b"\x00\xffroot")
            (source / "Nested" / "unicode-\u4e2d.txt").write_bytes(b"nested")
            opt = options(
                "--mode",
                "zip-hex",
                "--source",
                str(source),
            )
            self.assertEqual(opt.remote_output, ".\\source")
            data = tool.read_text_source(opt)
            self.assertEqual(data.source_kind, "directory")
            tool.validate_text(data, tool.analyze_text(data.text), opt)
            zipped = tool.zip_directory_payload(data)
            with zipfile.ZipFile(io.BytesIO(zipped), "r") as archive:
                self.assertEqual(archive.read("root.bin"), b"\x00\xffroot")
                self.assertEqual(archive.read("Nested/unicode-\u4e2d.txt"), b"nested")
                self.assertIn("empty/", archive.namelist())
                self.assertTrue(all(not name.startswith("source/") for name in archive.namelist()))
            payload, _, _ = tool.hex_payload_from_bytes(zipped, opt.hex_chunk_bytes)
            commands = tool.build_zip_hex_commands(payload, opt, directory_source=True)
            assert_safe_stream(self, commands)
            self.assertIn("certutil -hashfile 'tt.zip' sha256", commands)
            self.assertIn("expand-archive -force 'tt.zip' '.\\source'", commands)
            self.assertIn("remove-item -force 'tt.hex'", commands)
            self.assertIn("remove-item -force 'tt.zip'", commands)

    def test_directory_source_is_zip_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            source.mkdir()
            for mode in ("simple", "cmd-hex"):
                opt = options("--mode", mode, "--source", str(source))
                with self.assertRaisesRegex(RuntimeError, "only supported with --mode zip-hex"):
                    tool.read_text_source(opt)

    def test_empty_binary_cmd_creates_empty_output_without_decodehex(self) -> None:
        opt = options("--mode", "cmd-hex", "--output-encoding", "preserve", "--remote-output", "empty.bin")
        commands = tool.build_cmd_hex_commands("\n", opt)
        assert_safe_stream(self, commands)
        self.assertIn("set-content -nonewline '.\\empty.bin' ''", commands)
        self.assertNotIn("decodehex", commands)

    def test_remote_output_combines_name_relative_and_absolute_paths(self) -> None:
        renamed = options("--mode", "cmd-hex", "--remote-output", "renamed.bin")
        self.assertEqual(renamed.remote_output, ".\\renamed.bin")

        relative = options(
            "--mode",
            "cmd-hex",
            "--source",
            "input.bin",
            "--output-encoding",
            "preserve",
            "--remote-output",
            "../",
        )
        self.assertEqual(relative.remote_output, "..\\input.bin")
        payload, _, _ = tool.hex_payload_from_bytes(b"data", relative.hex_chunk_bytes)
        relative_commands = tool.build_cmd_hex_commands(payload, relative)
        assert_safe_stream(self, relative_commands)
        self.assertIn("new-item -itemtype directory -force '..'", relative_commands)
        self.assertIn("'..\\input.bin'", relative_commands)
        self.assertNotIn("tt.cmd", relative_commands)

        absolute = options(
            "--mode",
            "cmd-hex",
            "--source",
            "input.bin",
            "--output-encoding",
            "preserve",
            "--remote-output",
            "C://Drop Folder/Output.BIN",
        )
        self.assertEqual(absolute.remote_output, "C:\\Drop Folder\\Output.BIN")
        absolute_commands = tool.build_cmd_hex_commands(payload, absolute)
        assert_safe_stream(self, absolute_commands)
        self.assertNotIn("C:", absolute_commands)
        self.assertNotIn("Drop Folder", absolute_commands)
        self.assertIn("certutil -f -decodehex 'tt.cmd.hex' 'tt.cmd'", absolute_commands)
        self.assertIn("cmd /d /c tt.cmd", absolute_commands)
        helper = tool.build_remote_helper(absolute, "cmd-hex").decode("utf-8")
        self.assertIn("C:\\Drop Folder\\Output.BIN", helper)

        quoted = options("--mode", "zip-hex", "--remote-output", "O'Brien.BIN")
        quoted_commands = tool.build_zip_hex_commands(payload, quoted)
        assert_safe_stream(self, quoted_commands)
        quoted_helper = tool.build_remote_helper(quoted, "zip-hex").decode("utf-8")
        self.assertIn(".\\O''Brien.BIN", quoted_helper)

        trailing = options(
            "--mode",
            "zip-hex",
            "--source",
            "input.bin",
            "--remote-output",
            "C://Drop Folder/",
        )
        self.assertEqual(trailing.remote_output, "C:\\Drop Folder\\input.bin")

    def test_remote_output_rejects_invalid_windows_targets(self) -> None:
        for invalid in (
            "",
            "c:relative.txt",
            "bad?.txt",
            "dir/nul.txt",
            "dir/trailing.",
            "//server/share",
            "tt.hex",
            "tt.zip",
            "tt.out",
            "tt.cmd",
            "tt.cmd.hex",
            "tt.hex/output.bin",
            "folder/TT.CMD/output.bin",
        ):
            with self.assertRaises(SystemExit, msg=invalid):
                options("--mode", "cmd-hex", "--remote-output", invalid)

    def test_simple_mode_is_strictly_shift_free(self) -> None:
        opt = options("--mode", "simple")
        safe = text_data("abc 123-=[]\\;',./\n")
        tool.validate_text(safe, tool.analyze_text(safe.text), opt)
        for unsafe in ("A", "_", "@", "#", "$", "中"):
            data = text_data(unsafe)
            with self.assertRaisesRegex(RuntimeError, "Simple mode only accepts"):
                tool.validate_text(data, tool.analyze_text(data.text), opt)

    def test_complex_mode_forces_physical_enter(self) -> None:
        for extra in ((), ("--ascii-keys",)):
            opt = options(
                "--mode",
                "cmd-hex",
                "--enter-mode",
                "unicode",
                "--remote-output",
                "C:/work/output.txt",
                *extra,
            )
            key_opt = tool.keyboard_opt_for_generated_ascii(opt)
            self.assertEqual(key_opt.enter_mode, "key")
            self.assertFalse(key_opt.remote_output_set)

    def test_windows_special_paths_are_rejected(self) -> None:
        for path in ("-foo", "dir/-foo", "con", "nul.txt", "com1.log", "a.", "../a", "/abs"):
            self.assertFalse(tool.is_safe_cmd_hex_path(path), path)
        for path in ("trans.txt", "dir/trans-1.txt", "01.txt"):
            self.assertTrue(tool.is_safe_cmd_hex_path(path), path)

    @unittest.skipUnless(os.name == "nt", "requires Windows PowerShell 5.1 and certutil")
    def test_python_protocol_roundtrips_on_windows_powershell(self) -> None:
        raw = "\t123 @#$ 中文\n".encode("utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for mode in ("cmd-hex", "zip-hex"):
                target = f"nested/{mode}/目标 文件.txt"
                opt = options(
                    "--mode",
                    mode,
                    "--remote-output",
                    target,
                )
                transfer = tool.zip_payload(raw, opt) if mode == "zip-hex" else raw
                payload, _, _ = tool.hex_payload_from_bytes(transfer, opt.hex_chunk_bytes)
                commands = (
                    tool.build_zip_hex_commands(payload, opt)
                    if mode == "zip-hex"
                    else tool.build_cmd_hex_commands(payload, opt)
                )
                execute_command_stream(commands, root)
                self.assertEqual((root / Path(target)).read_bytes(), raw)
                self.assertFalse((root / tool.REMOTE_HEX).exists())
                self.assertFalse((root / tool.REMOTE_ZIP).exists())
                self.assertFalse((root / tool.REMOTE_STAGE).exists())
                self.assertFalse((root / tool.REMOTE_HELPER_HEX).exists())
                self.assertFalse((root / tool.REMOTE_HELPER).exists())


if __name__ == "__main__":
    unittest.main()

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
                self.assertNotIn("del ", commands)
                self.assertIn("certutil -hashfile 'trans.txt' sha256", commands)

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

    def test_preserve_rejects_clipboard_data(self) -> None:
        data = text_data("text")
        data.raw_bytes = None
        opt = options("--mode", "cmd-hex", "--source", "clipboard", "--output-encoding", "preserve")
        with self.assertRaisesRegex(RuntimeError, "requires file input"):
            tool.validate_text(data, tool.analyze_text(data.text), opt)

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
            opt = options("--mode", "cmd-hex", "--enter-mode", "unicode", *extra)
            key_opt = tool.keyboard_opt_for_generated_ascii(opt)
            self.assertEqual(key_opt.enter_mode, "key")

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
                opt = options(
                    "--mode",
                    mode,
                    "--remote-output",
                    f"out-{mode}.txt",
                    "--remote-hex",
                    f"temp-{mode}.hex",
                    "--remote-zip",
                    f"temp-{mode}.zip",
                )
                transfer = tool.zip_payload(raw, opt) if mode == "zip-hex" else raw
                payload, _, _ = tool.hex_payload_from_bytes(transfer, opt.hex_chunk_bytes)
                commands = (
                    tool.build_zip_hex_commands(payload, opt)
                    if mode == "zip-hex"
                    else tool.build_cmd_hex_commands(payload, opt)
                )
                execute_command_stream(commands, root)
                self.assertEqual((root / opt.remote_output).read_bytes(), raw)
                self.assertTrue((root / opt.remote_hex).exists())
                if mode == "zip-hex":
                    self.assertTrue((root / opt.remote_zip).exists())


if __name__ == "__main__":
    unittest.main()

import argparse
import re
import subprocess
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from test_protocol import ALLOWED_COMMAND_CHARS, execute_command_stream
import trans_type as protocol


HEX_LINE = re.compile(r"^(?:set|add)-content -encoding ascii '([^']+)' '([0-9a-f]+)'$")


def payload_from_commands(commands: str, destination: str = protocol.REMOTE_HEX) -> bytes:
    chunks = []
    for line in commands.splitlines():
        match = HEX_LINE.fullmatch(line)
        if match and match.group(1) == destination:
            chunks.append(match.group(2))
    if not chunks:
        raise AssertionError(f"generated command stream contains no hex payload for {destination}")
    return bytes.fromhex("".join(chunks))


PE_MACHINES = {
    "x86": 0x014C,
    "x64": 0x8664,
}


def verify_pe_architecture(executable: Path, expected_architecture: str) -> None:
    data = executable.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise AssertionError(f"{executable.name} is not a PE executable")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 6 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise AssertionError(f"{executable.name} has an invalid PE header")
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    expected_machine = PE_MACHINES[expected_architecture]
    if machine != expected_machine:
        raise AssertionError(
            f"{executable.name} is not {expected_architecture} "
            f"(expected PE machine 0x{expected_machine:04x}, got 0x{machine:04x})"
        )


def verify_help(executable: Path, root: Path) -> None:
    result = subprocess.run([str(executable), "--help"], cwd=root, capture_output=True, check=True)
    help_text = result.stdout.decode("utf-8")
    required = (
        "选项 / options:",
        "使用方法和示例 / usage and examples:",
        "--source SOURCE",
        "--remote-output TARGET",
        "--output-encoding E",
        "--mode simple [--source clipboard|file|PATH]",
        "--mode cmd-hex --source PATH --output-encoding preserve",
        "--mode zip-hex --source FOLDER [--remote-output TARGET]",
        "运行控制 / runtime controls:",
        "Esc",
    )
    missing = [text for text in required if text not in help_text]
    if missing:
        raise AssertionError(f"{executable.name} bilingual help is incomplete: {missing}")
    for removed in ("--file", "--remote-hex", "--remote-zip", "--remote-path"):
        if removed in help_text:
            raise AssertionError(f"{executable.name} help still advertises removed option {removed}")
    for verbose in ("模式、内容与文件", "允许/allowed", "intermediates:"):
        if verbose in help_text:
            raise AssertionError(f"{executable.name} help still contains verbose section {verbose}")
    if "Space" in help_text or "Paused. Press Space" in help_text:
        raise AssertionError(f"{executable.name} help still advertises the removed Space pause control")


def verify_removed_options(executable: Path, root: Path) -> None:
    for option in ("--file", "--remote-hex", "--remote-zip", "--remote-path"):
        result = subprocess.run(
            [str(executable), option, "unused"],
            cwd=root,
            capture_output=True,
            text=True,
        )
        if result.returncode != 2:
            raise AssertionError(
                f"{executable.name} did not reject removed option {option}; "
                f"exit={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}"
            )


def verify_argument_validation(executable: Path, source: Path, root: Path) -> None:
    cases = (
        ("--mode", "simple", "--source", str(source), "--remote-output", "output.txt", "--dry-run"),
        ("--mode", "simple", "--source", str(source), "--commands-out", str(root / "bad.commands"), "--dry-run"),
        ("--mode", "cmd-hex", "--source", str(source), "--remote-output", "c:relative.txt", "--dry-run"),
        ("--mode", "cmd-hex", "--source", str(source), "--remote-output", "bad?.txt", "--dry-run"),
        ("--mode", "cmd-hex", "--source", str(source), "--remote-output", "dir/nul.txt", "--dry-run"),
        ("--mode", "cmd-hex", "--source", str(source), "--remote-output", "tt.hex/output.txt", "--dry-run"),
    )
    for arguments in cases:
        result = subprocess.run(
            [str(executable), *arguments],
            cwd=root,
            capture_output=True,
            text=True,
        )
        if result.returncode != 2:
            raise AssertionError(
                f"{executable.name} returned {result.returncode} for invalid arguments {arguments}\n"
                f"stdout={result.stdout}\nstderr={result.stderr}"
            )


def verify(
    executable: Path,
    label: str,
    mode: str,
    encoding: str,
    source: Path,
    root: Path,
    expected: bytes,
    remote_output: str | None = None,
) -> None:
    suffix = f"{label}-{mode}-{encoding}"
    target = protocol.normalize_remote_output(remote_output, source.name)
    if target is None:
        raise AssertionError("test supplied an invalid remote output")
    command_file = root / f"commands-{suffix}.txt"
    arguments = [
        str(executable),
        "--mode",
        mode,
        "--source",
        str(source),
        "--output-encoding",
        encoding,
        "--dry-run",
        "--commands-out",
        str(command_file),
    ]
    if remote_output is not None:
        arguments.extend(("--remote-output", remote_output))
    for temporary in (
        root / protocol.REMOTE_HEX,
        root / protocol.REMOTE_ZIP,
        root / protocol.REMOTE_STAGE,
        root / protocol.REMOTE_HELPER_HEX,
        root / protocol.REMOTE_HELPER,
    ):
        temporary.unlink(missing_ok=True)
    subprocess.run(arguments, check=True, cwd=root)
    commands = command_file.read_text(encoding="ascii")
    if not set(commands) <= ALLOWED_COMMAND_CHARS or any("A" <= ch <= "Z" for ch in commands):
        raise AssertionError(f"{executable.name} generated a forbidden command character")
    if "remove-item -force 'tt.hex'" not in commands:
        raise AssertionError(f"{executable.name} does not clean up tt.hex")
    if mode == "zip-hex" and "remove-item -force 'tt.zip'" not in commands:
        raise AssertionError(f"{executable.name} does not clean up tt.zip")
    execute_command_stream(commands, root)
    if (root / target).read_bytes() != expected:
        raise AssertionError(f"{executable.name} {mode} output differs from the source")
    for temporary in (
        protocol.REMOTE_HEX,
        protocol.REMOTE_ZIP,
        protocol.REMOTE_STAGE,
        protocol.REMOTE_HELPER_HEX,
        protocol.REMOTE_HELPER,
    ):
        if (root / temporary).exists():
            raise AssertionError(f"{executable.name} left {temporary} behind")


def verify_simple_rejections(executable: Path, root: Path) -> None:
    for index, raw in enumerate((b"A\n", b"@\n", "中文\n".encode("utf-8"))):
        source = root / f"invalid-simple-{index}.txt"
        source.write_bytes(raw)
        result = subprocess.run(
            [str(executable), "--mode", "simple", "--source", str(source), "--dry-run"],
            cwd=root,
            capture_output=True,
            text=True,
        )
        if result.returncode != 5:
            raise AssertionError(
                f"{executable.name} accepted invalid simple input {raw!r}; "
                f"exit={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}"
            )


def verify_remote_output_commands(executable: Path, label: str, source: Path, root: Path) -> None:
    command_file = root / f"remote-output-{label}.commands"
    subprocess.run(
        [
            str(executable),
            "--mode",
            "cmd-hex",
            "--source",
            str(source),
            "--remote-output",
            "nested/lower/",
            "--dry-run",
            "--commands-out",
            str(command_file),
        ],
        check=True,
        cwd=root,
    )
    commands = command_file.read_text(encoding="ascii")
    if not set(commands) <= ALLOWED_COMMAND_CHARS or any("A" <= ch <= "Z" for ch in commands):
        raise AssertionError(f"{executable.name} generated a forbidden remote-output command character")
    if "new-item -itemtype directory -force 'nested\\lower'" not in commands:
        raise AssertionError(f"{executable.name} did not create the requested remote directory")
    target = f"nested\\lower\\{source.name}"
    if f"'{target}'" not in commands:
        raise AssertionError(f"{executable.name} did not append the source basename to the directory target")
    if protocol.REMOTE_HELPER in commands:
        raise AssertionError(f"{executable.name} unexpectedly used a helper for a shift-free target")
    execute_command_stream(commands, root)
    if (root / target).read_bytes() != source.read_bytes():
        raise AssertionError(f"{executable.name} direct relative target differs from the source")


def verify_helper_targets(executable: Path, label: str, source: Path, root: Path) -> None:
    target_arg = "Nested Folder/O'Brien.BIN"
    target = "Nested Folder\\O'Brien.BIN"
    for mode in ("cmd-hex", "zip-hex"):
        command_file = root / f"helper-{label}-{mode}.commands"
        subprocess.run(
            [
                str(executable),
                "--mode",
                mode,
                "--source",
                str(source),
                "--remote-output",
                target_arg,
                "--dry-run",
                "--commands-out",
                str(command_file),
            ],
            check=True,
            cwd=root,
        )
        commands = command_file.read_text(encoding="ascii")
        if not set(commands) <= ALLOWED_COMMAND_CHARS or any("A" <= ch <= "Z" for ch in commands):
            raise AssertionError(f"{executable.name} generated a forbidden helper command character")
        if target in commands or target_arg in commands:
            raise AssertionError(f"{executable.name} leaked the complex target into the outer command stream")
        if "cmd /d /c tt.cmd" not in commands:
            raise AssertionError(f"{executable.name} did not use the helper for a complex target")
        helper = payload_from_commands(commands, protocol.REMOTE_HELPER_HEX).decode("utf-8")
        if target.replace("'", "''") not in helper:
            raise AssertionError(f"{executable.name} helper does not contain the normalized target")
        execute_command_stream(commands, root)
        if (root / target).read_bytes() != source.read_bytes():
            raise AssertionError(f"{executable.name} {mode} helper output differs from the source")
        for temporary in (
            protocol.REMOTE_HEX,
            protocol.REMOTE_ZIP,
            protocol.REMOTE_STAGE,
            protocol.REMOTE_HELPER_HEX,
            protocol.REMOTE_HELPER,
        ):
            if (root / temporary).exists():
                raise AssertionError(f"{executable.name} left {temporary} after helper transfer")


def verify_zip_compression(executable: Path, label: str, root: Path) -> None:
    raw = b"repetitive line 0123456789\n" * 2000
    source = root / f"compress-{label}.txt"
    command_file = root / f"compress-{label}.commands"
    source.write_bytes(raw)
    subprocess.run(
        [
            str(executable),
            "--mode",
            "zip-hex",
            "--source",
            str(source),
            "--remote-output",
            f"compress-{label}.txt",
            "--dry-run",
            "--commands-out",
            str(command_file),
        ],
        check=True,
        cwd=root,
    )
    commands = command_file.read_text(encoding="ascii")
    payload = payload_from_commands(commands)
    if len(payload) >= len(raw) // 10:
        raise AssertionError(
            f"{executable.name} zip payload is not effectively compressed: {len(payload)} >= {len(raw) // 10}"
        )


def verify_directory_transfer(executable: Path, label: str, root: Path) -> None:
    source = root / "folder-inputs" / f"folder-source-{label}"
    (source / "Nested").mkdir(parents=True)
    (source / "empty").mkdir()
    (source / "root.bin").write_bytes(b"\x00\xffroot")
    (source / "Nested" / "unicode-\u4e2d.txt").write_bytes(b"nested")
    destination = source.name
    command_file = root / f"folder-{label}.commands"
    subprocess.run(
        [
            str(executable),
            "--mode",
            "zip-hex",
            "--source",
            str(source),
            "--dry-run",
            "--commands-out",
            str(command_file),
        ],
        check=True,
        cwd=root,
    )
    commands = command_file.read_text(encoding="ascii")
    if not set(commands) <= ALLOWED_COMMAND_CHARS or any("A" <= ch <= "Z" for ch in commands):
        raise AssertionError(f"{executable.name} generated a forbidden folder command character")
    expected_expand = f"expand-archive -force 'tt.zip' '.\\{destination}'"
    if expected_expand not in commands or "certutil -hashfile 'tt.zip' sha256" not in commands:
        raise AssertionError(f"{executable.name} generated an invalid folder reconstruction protocol")
    for temporary in (root / "tt.hex", root / "tt.zip"):
        temporary.unlink(missing_ok=True)
    execute_command_stream(commands, root)
    copied = root / destination
    if (copied / "root.bin").read_bytes() != b"\x00\xffroot":
        raise AssertionError(f"{executable.name} folder root file differs")
    if (copied / "Nested" / "unicode-\u4e2d.txt").read_bytes() != b"nested":
        raise AssertionError(f"{executable.name} folder nested file differs")
    if not (copied / "empty").is_dir():
        raise AssertionError(f"{executable.name} folder transfer lost an empty directory")
    if (copied / source.name).exists():
        raise AssertionError(f"{executable.name} unexpectedly nested the local source root")
    if (root / "tt.hex").exists() or (root / "tt.zip").exists():
        raise AssertionError(f"{executable.name} left folder-transfer temporary files behind")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected-arch", choices=tuple(PE_MACHINES), default="x64")
    parser.add_argument("executables", metavar="EXE", type=Path, nargs="+")
    args = parser.parse_args()
    executables = [path.resolve() for path in args.executables]
    text = "\t123 @#$ 中文\n"
    utf8_source = text.encode("utf-8")
    preserve_source = b"\xff\xfe" + text.encode("utf-16-le")
    binary_source = bytes(range(256)) * 4
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        inputs = root / "inputs"
        inputs.mkdir()
        source_utf8 = inputs / "input-utf8.txt"
        source_preserve = inputs / "input-preserve.txt"
        source_binary = inputs / "input-arbitrary.bin"
        source_utf8.write_bytes(utf8_source)
        source_preserve.write_bytes(preserve_source)
        source_binary.write_bytes(binary_source)
        for index, executable in enumerate(executables):
            verify_pe_architecture(executable, args.expected_arch)
            verify_help(executable, root)
            verify_removed_options(executable, root)
            verify_argument_validation(executable, source_utf8, root)
            verify_simple_rejections(executable, root)
            label = f"e{index}"
            verify_remote_output_commands(executable, label, source_utf8, root)
            verify_helper_targets(executable, label, source_utf8, root)
            verify_zip_compression(executable, label, root)
            verify_directory_transfer(executable, label, root)
            for mode in ("cmd-hex", "zip-hex"):
                verify(executable, label, mode, "utf8", source_utf8, root, utf8_source)
                verify(
                    executable,
                    label,
                    mode,
                    "utf8-bom",
                    source_utf8,
                    root,
                    b"\xef\xbb\xbf" + utf8_source,
                    f"out-{label}-{mode}-bom.txt",
                )
                verify(executable, label, mode, "preserve", source_preserve, root, preserve_source)
                verify(executable, f"{label}-binary", mode, "preserve", source_binary, root, binary_source)
    print("Windows exe protocol roundtrips passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

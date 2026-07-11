import re
import subprocess
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from test_protocol import ALLOWED_COMMAND_CHARS, execute_command_stream


HEX_LINE = re.compile(r"^(?:set|add)-content -encoding ascii '[^']+' '([0-9a-f]+)'$")


def payload_from_commands(commands: str) -> bytes:
    chunks = []
    for line in commands.splitlines():
        match = HEX_LINE.fullmatch(line)
        if match:
            chunks.append(match.group(1))
    if not chunks:
        raise AssertionError("generated command stream contains no hex payload")
    return bytes.fromhex("".join(chunks))


def verify_pe_x64(executable: Path) -> None:
    data = executable.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise AssertionError(f"{executable.name} is not a PE executable")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 6 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise AssertionError(f"{executable.name} has an invalid PE header")
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    if machine != 0x8664:
        raise AssertionError(f"{executable.name} is not x64 (PE machine 0x{machine:04x})")


def verify_help(executable: Path, root: Path) -> None:
    result = subprocess.run([str(executable), "--help"], cwd=root, capture_output=True, check=True)
    help_text = result.stdout.decode("utf-8")
    required = (
        "选项 / options:",
        "模式、内容与文件 / modes, content, and files:",
        "--source SOURCE",
        "--remote-output NAME",
        "--remote-path PATH",
        "--output-encoding E",
        "直接输入，不创建远端文件",
        "output follows source; temp deleted",
        "source-named folder; temp deleted",
        "Space",
    )
    missing = [text for text in required if text not in help_text]
    if missing:
        raise AssertionError(f"{executable.name} bilingual help is incomplete: {missing}")
    for removed in ("--file", "--remote-hex", "--remote-zip"):
        if removed in help_text:
            raise AssertionError(f"{executable.name} help still advertises removed option {removed}")


def verify_removed_options(executable: Path, root: Path) -> None:
    for option in ("--file", "--remote-hex", "--remote-zip"):
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
    unsafe_basename = source.parent / "Upper.TXT"
    unsafe_basename.write_bytes(source.read_bytes())
    cases = (
        ("--mode", "simple", "--source", str(source), "--remote-output", "output.txt", "--dry-run"),
        ("--mode", "simple", "--source", str(source), "--commands-out", str(root / "bad.commands"), "--dry-run"),
        ("--mode", "cmd-hex", "--source", str(source), "--remote-output", "dir\\out.txt", "--dry-run"),
        ("--mode", "cmd-hex", "--source", str(source), "--remote-path", "c:\\work", "--dry-run"),
        ("--mode", "cmd-hex", "--source", str(unsafe_basename), "--dry-run"),
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
    output = remote_output or source.name
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
    for temporary in (root / "tt.hex", root / "tt.zip"):
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
    if (root / output).read_bytes() != expected:
        raise AssertionError(f"{executable.name} {mode} output differs from the source")
    if (root / "tt.hex").exists():
        raise AssertionError(f"{executable.name} left tt.hex behind")
    if mode == "zip-hex" and (root / "tt.zip").exists():
        raise AssertionError(f"{executable.name} left tt.zip behind")


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


def verify_remote_path_commands(executable: Path, label: str, source: Path, root: Path) -> None:
    command_file = root / f"remote-path-{label}.commands"
    subprocess.run(
        [
            str(executable),
            "--mode",
            "cmd-hex",
            "--source",
            str(source),
            "--remote-path",
            "\\work\\drop",
            "--dry-run",
            "--commands-out",
            str(command_file),
        ],
        check=True,
        cwd=root,
    )
    commands = command_file.read_text(encoding="ascii")
    if not set(commands) <= ALLOWED_COMMAND_CHARS or any("A" <= ch <= "Z" for ch in commands):
        raise AssertionError(f"{executable.name} generated a forbidden remote-path command character")
    if "new-item -itemtype directory -force '\\work\\drop'" not in commands:
        raise AssertionError(f"{executable.name} did not create the requested remote directory")
    if f"'\\work\\drop\\{source.name}'" not in commands:
        raise AssertionError(f"{executable.name} did not combine remote path with source basename")


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
    expected_expand = f"expand-archive -force 'tt.zip' '{destination}'"
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
    executables = [Path(arg).resolve() for arg in sys.argv[1:]]
    if not executables:
        raise SystemExit("usage: verify_windows_exes.py EXE [EXE ...]")
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
            verify_pe_x64(executable)
            verify_help(executable, root)
            verify_removed_options(executable, root)
            verify_argument_validation(executable, source_utf8, root)
            verify_simple_rejections(executable, root)
            label = f"e{index}"
            verify_remote_path_commands(executable, label, source_utf8, root)
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

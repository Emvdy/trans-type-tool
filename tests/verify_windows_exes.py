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


def verify(
    executable: Path,
    label: str,
    mode: str,
    encoding: str,
    source: Path,
    root: Path,
    expected: bytes,
) -> None:
    suffix = f"{label}-{mode}-{encoding}"
    output = f"out-{suffix}.txt"
    remote_hex = f"temp-{suffix}.hex"
    remote_zip = f"temp-{suffix}.zip"
    command_file = root / f"commands-{suffix}.txt"
    subprocess.run(
        [
            str(executable),
            "--mode",
            mode,
            "--file",
            str(source),
            "--remote-output",
            output,
            "--remote-hex",
            remote_hex,
            "--remote-zip",
            remote_zip,
            "--output-encoding",
            encoding,
            "--dry-run",
            "--commands-out",
            str(command_file),
        ],
        check=True,
        cwd=root,
    )
    commands = command_file.read_text(encoding="ascii")
    if not set(commands) <= ALLOWED_COMMAND_CHARS or any("A" <= ch <= "Z" for ch in commands):
        raise AssertionError(f"{executable.name} generated a forbidden command character")
    execute_command_stream(commands, root)
    if (root / output).read_bytes() != expected:
        raise AssertionError(f"{executable.name} {mode} output differs from the source")
    if not (root / remote_hex).exists():
        raise AssertionError(f"{executable.name} did not retain the remote hex file")
    if mode == "zip-hex" and not (root / remote_zip).exists():
        raise AssertionError(f"{executable.name} did not retain the remote zip file")


def verify_simple_rejections(executable: Path, root: Path) -> None:
    for index, raw in enumerate((b"A\n", b"@\n", "中文\n".encode("utf-8"))):
        source = root / f"invalid-simple-{index}.txt"
        source.write_bytes(raw)
        result = subprocess.run(
            [str(executable), "--mode", "simple", "--file", str(source), "--dry-run"],
            cwd=root,
            capture_output=True,
            text=True,
        )
        if result.returncode != 5:
            raise AssertionError(
                f"{executable.name} accepted invalid simple input {raw!r}; "
                f"exit={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}"
            )


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
            "--file",
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


def main() -> int:
    executables = [Path(arg).resolve() for arg in sys.argv[1:]]
    if not executables:
        raise SystemExit("usage: verify_windows_exes.py EXE [EXE ...]")
    text = "\t123 @#$ 中文\n"
    utf8_source = text.encode("utf-8")
    preserve_source = b"\xff\xfe" + text.encode("utf-16-le")
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        source_utf8 = root / "input-utf8.txt"
        source_preserve = root / "input-preserve.txt"
        source_utf8.write_bytes(utf8_source)
        source_preserve.write_bytes(preserve_source)
        for index, executable in enumerate(executables):
            verify_pe_x64(executable)
            verify_simple_rejections(executable, root)
            label = f"e{index}"
            verify_zip_compression(executable, label, root)
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
                )
                verify(executable, label, mode, "preserve", source_preserve, root, preserve_source)
    print("Windows exe protocol roundtrips passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

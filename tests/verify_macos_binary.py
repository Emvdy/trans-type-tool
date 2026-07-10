import io
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import trans_type as protocol


HEX_LINE = re.compile(r"^(?:set|add)-content -encoding ascii '[^']+' '([0-9a-f]+)'$")


def run_checked(arguments: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(arguments, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        raise AssertionError(
            f"command failed with exit {result.returncode}: {arguments}\n"
            f"stdout={result.stdout}\nstderr={result.stderr}"
        )
    return result


def payload_from_commands(commands: str) -> bytes:
    chunks = []
    for line in commands.splitlines():
        match = HEX_LINE.fullmatch(line)
        if match:
            chunks.append(match.group(1))
    if not chunks:
        raise AssertionError("generated command stream contains no hex payload")
    return bytes.fromhex("".join(chunks))


def verify_mode(
    executable: Path,
    root: Path,
    mode: str,
    encoding: str,
    source: Path,
    expected: bytes,
) -> int:
    command_file = root / f"commands-{mode}-{encoding}.txt"
    remote_output = f"out-{mode}-{encoding}.txt"
    run_checked(
        [
            str(executable),
            "--mode",
            mode,
            "--file",
            str(source),
            "--remote-output",
            remote_output,
            "--output-encoding",
            encoding,
            "--commands-out",
            str(command_file),
            "--dry-run",
        ],
        root,
    )
    commands = command_file.read_text(encoding="ascii")
    protocol.validate_generated_command_stream(commands)
    payload = payload_from_commands(commands)
    if mode == "cmd-hex":
        actual = payload
    else:
        with zipfile.ZipFile(io.BytesIO(payload), "r") as archive:
            if archive.namelist() != [remote_output]:
                raise AssertionError(f"unexpected zip entries: {archive.namelist()}")
            actual = archive.read(remote_output)
    if actual != expected:
        raise AssertionError(f"{mode}/{encoding} payload differs from expected bytes")
    return len(payload)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_macos_binary.py EXECUTABLE")
    executable = Path(sys.argv[1]).resolve()
    text = "\t123 @#$ 中文\n"
    utf8 = text.encode("utf-8")
    preserved = b"\xff\xfe" + text.encode("utf-16-le")

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        safe = root / "safe.txt"
        complex_source = root / "complex.txt"
        preserve_source = root / "preserve.txt"
        safe.write_text("abc 123-=[]\\;',./\n", encoding="ascii")
        complex_source.write_bytes(utf8)
        preserve_source.write_bytes(preserved)

        run_checked([str(executable), "--mode", "simple", "--file", str(safe), "--dry-run"], root)
        rejected = subprocess.run(
            [str(executable), "--mode", "simple", "--file", str(complex_source), "--dry-run"],
            cwd=root,
            capture_output=True,
            text=True,
        )
        if rejected.returncode != 5:
            raise AssertionError(f"simple mode accepted complex input; exit={rejected.returncode}")

        for mode in ("cmd-hex", "zip-hex"):
            verify_mode(executable, root, mode, "utf8", complex_source, utf8)
            verify_mode(executable, root, mode, "utf8-bom", complex_source, b"\xef\xbb\xbf" + utf8)
            verify_mode(executable, root, mode, "preserve", preserve_source, preserved)

        repetitive = b"repetitive line 0123456789\n" * 2000
        repetitive_source = root / "repetitive.txt"
        repetitive_source.write_bytes(repetitive)
        compressed_size = verify_mode(executable, root, "zip-hex", "utf8", repetitive_source, repetitive)
        if compressed_size >= len(repetitive) // 10:
            raise AssertionError(
                f"macOS zip payload is not effectively compressed: {compressed_size} >= {len(repetitive) // 10}"
            )

    print("macOS binary protocol payloads passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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


def verify_help(executable: Path, root: Path) -> None:
    result = subprocess.run([str(executable), "--help"], cwd=root, capture_output=True, check=True)
    help_text = result.stdout.decode("utf-8")
    required = (
        "选项 / options:",
        "模式、内容与文件 / modes, content, and files:",
        "--source SOURCE",
        "--output-encoding E",
        "--request-accessibility",
        "直接输入，不创建远端文件",
        "输出 trans.txt，临时 tt.hex",
        "临时 tt.hex、tt.zip",
        "destination folder; temporary files",
    )
    missing = [text for text in required if text not in help_text]
    if missing:
        raise AssertionError(f"macOS bilingual help is incomplete: {missing}")


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
            "--source",
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


def verify_directory(executable: Path, root: Path) -> None:
    source = root / "folder-source"
    (source / "Nested").mkdir(parents=True)
    (source / "empty").mkdir()
    (source / "root.bin").write_bytes(b"\x00\xffroot")
    (source / "Nested" / "unicode-\u4e2d.txt").write_bytes(b"nested")
    command_file = root / "commands-folder.txt"
    run_checked(
        [
            str(executable),
            "--mode",
            "zip-hex",
            "--source",
            str(source),
            "--remote-output",
            "copied-dir",
            "--commands-out",
            str(command_file),
            "--dry-run",
        ],
        root,
    )
    commands = command_file.read_text(encoding="ascii")
    protocol.validate_generated_command_stream(commands)
    if "certutil -hashfile 'tt.zip' sha256" not in commands:
        raise AssertionError("folder command stream does not verify the transferred archive")
    if "expand-archive -force 'tt.zip' 'copied-dir'" not in commands:
        raise AssertionError("folder command stream does not use the requested destination directory")
    payload = payload_from_commands(commands)
    with zipfile.ZipFile(io.BytesIO(payload), "r") as archive:
        names = archive.namelist()
        if archive.read("root.bin") != b"\x00\xffroot":
            raise AssertionError("folder root file differs")
        if archive.read("Nested/unicode-\u4e2d.txt") != b"nested":
            raise AssertionError("folder nested file differs")
        if "empty/" not in names:
            raise AssertionError(f"folder archive lost empty directory: {names}")
        if any(name.startswith("folder-source/") for name in names):
            raise AssertionError("folder archive unexpectedly includes the local source root")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_macos_binary.py EXECUTABLE")
    executable = Path(sys.argv[1]).resolve()
    text = "\t123 @#$ 中文\n"
    utf8 = text.encode("utf-8")
    preserved = b"\xff\xfe" + text.encode("utf-16-le")

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        verify_help(executable, root)
        safe = root / "safe.txt"
        complex_source = root / "complex.txt"
        preserve_source = root / "preserve.txt"
        binary_source = root / "arbitrary.bin"
        safe.write_text("abc 123-=[]\\;',./\n", encoding="ascii")
        complex_source.write_bytes(utf8)
        preserve_source.write_bytes(preserved)
        binary_source.write_bytes(bytes(range(256)) * 4)

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
            verify_mode(executable, root, mode, "preserve", binary_source, binary_source.read_bytes())

        repetitive = b"repetitive line 0123456789\n" * 2000
        repetitive_source = root / "repetitive.txt"
        repetitive_source.write_bytes(repetitive)
        compressed_size = verify_mode(executable, root, "zip-hex", "utf8", repetitive_source, repetitive)
        if compressed_size >= len(repetitive) // 10:
            raise AssertionError(
                f"macOS zip payload is not effectively compressed: {compressed_size} >= {len(repetitive) // 10}"
            )

        verify_directory(executable, root)

    print("macOS binary protocol payloads passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

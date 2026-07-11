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
        "--remote-output NAME",
        "--remote-path PATH",
        "--output-encoding E",
        "--request-accessibility",
        "直接输入，不创建远端文件",
        "output follows source; temp deleted",
        "source-named folder; temp deleted",
        "Space",
    )
    missing = [text for text in required if text not in help_text]
    if missing:
        raise AssertionError(f"macOS bilingual help is incomplete: {missing}")
    for removed in ("--file", "--remote-hex", "--remote-zip"):
        if removed in help_text:
            raise AssertionError(f"macOS help still advertises removed option {removed}")


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
                f"macOS binary did not reject removed option {option}; "
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
                f"macOS binary returned {result.returncode} for invalid arguments {arguments}\n"
                f"stdout={result.stdout}\nstderr={result.stderr}"
            )


def verify_mode(
    executable: Path,
    root: Path,
    mode: str,
    encoding: str,
    source: Path,
    expected: bytes,
    remote_output: str | None = None,
) -> int:
    command_file = root / f"commands-{mode}-{encoding}.txt"
    output = remote_output or source.name
    arguments = [
        str(executable),
        "--mode",
        mode,
        "--source",
        str(source),
        "--output-encoding",
        encoding,
        "--commands-out",
        str(command_file),
        "--dry-run",
    ]
    if remote_output is not None:
        arguments.extend(("--remote-output", remote_output))
    run_checked(arguments, root)
    commands = command_file.read_text(encoding="ascii")
    protocol.validate_generated_command_stream(commands)
    if "remove-item -force 'tt.hex'" not in commands:
        raise AssertionError("macOS command stream does not clean up tt.hex")
    if mode == "zip-hex" and "remove-item -force 'tt.zip'" not in commands:
        raise AssertionError("macOS zip command stream does not clean up tt.zip")
    if f"certutil -hashfile '{output}' sha256" not in commands:
        raise AssertionError(f"macOS command stream does not target the expected basename {output}")
    payload = payload_from_commands(commands)
    if mode == "cmd-hex":
        actual = payload
    else:
        with zipfile.ZipFile(io.BytesIO(payload), "r") as archive:
            if archive.namelist() != [output]:
                raise AssertionError(f"unexpected zip entries: {archive.namelist()}")
            actual = archive.read(output)
    if actual != expected:
        raise AssertionError(f"{mode}/{encoding} payload differs from expected bytes")
    return len(payload)


def verify_directory(executable: Path, root: Path) -> None:
    source = root / "folder-inputs" / "folder-source"
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
    if "expand-archive -force 'tt.zip' 'folder-source'" not in commands:
        raise AssertionError("folder command stream does not use the source basename as destination")
    if "remove-item -force 'tt.hex'" not in commands or "remove-item -force 'tt.zip'" not in commands:
        raise AssertionError("folder command stream does not clean up temporary files")
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


def verify_remote_path(executable: Path, root: Path, source: Path) -> None:
    command_file = root / "commands-remote-path.txt"
    run_checked(
        [
            str(executable),
            "--mode",
            "cmd-hex",
            "--source",
            str(source),
            "--remote-path",
            "\\work\\drop",
            "--commands-out",
            str(command_file),
            "--dry-run",
        ],
        root,
    )
    commands = command_file.read_text(encoding="ascii")
    protocol.validate_generated_command_stream(commands)
    if "new-item -itemtype directory -force '\\work\\drop'" not in commands:
        raise AssertionError("macOS command stream does not create the remote directory")
    if f"'\\work\\drop\\{source.name}'" not in commands:
        raise AssertionError("macOS command stream does not combine remote path and source basename")


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
        verify_removed_options(executable, root)
        inputs = root / "inputs"
        inputs.mkdir()
        safe = inputs / "safe.txt"
        complex_source = inputs / "complex.txt"
        preserve_source = inputs / "preserve.txt"
        binary_source = inputs / "arbitrary.bin"
        safe.write_text("abc 123-=[]\\;',./\n", encoding="ascii")
        complex_source.write_bytes(utf8)
        preserve_source.write_bytes(preserved)
        binary_source.write_bytes(bytes(range(256)) * 4)

        verify_argument_validation(executable, complex_source, root)

        run_checked([str(executable), "--mode", "simple", "--source", str(safe), "--dry-run"], root)
        rejected = subprocess.run(
            [str(executable), "--mode", "simple", "--source", str(complex_source), "--dry-run"],
            cwd=root,
            capture_output=True,
            text=True,
        )
        if rejected.returncode != 5:
            raise AssertionError(f"simple mode accepted complex input; exit={rejected.returncode}")

        verify_remote_path(executable, root, complex_source)

        for mode in ("cmd-hex", "zip-hex"):
            verify_mode(executable, root, mode, "utf8", complex_source, utf8)
            verify_mode(
                executable,
                root,
                mode,
                "utf8-bom",
                complex_source,
                b"\xef\xbb\xbf" + utf8,
                f"out-{mode}-bom.txt",
            )
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

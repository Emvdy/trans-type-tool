import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import trans_type as tool


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit("usage: verify_command_files.py FILE [FILE ...]")
    for name in sys.argv[1:]:
        path = Path(name)
        commands = path.read_text(encoding="ascii")
        tool.validate_generated_command_stream(commands)
        if "certutil -hashfile" not in commands:
            raise AssertionError(f"{path} does not verify the remote output hash")
        if "remove-item -force 'tt.hex'" not in commands:
            raise AssertionError(f"{path} does not clean up tt.hex")
        if "decodehex 'tt.hex' 'tt.zip'" in commands and "remove-item -force 'tt.zip'" not in commands:
            raise AssertionError(f"{path} does not clean up tt.zip")
    print(f"Verified {len(sys.argv) - 1} generated command stream(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

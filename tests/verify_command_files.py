import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import trans_type as tool


HEX_LINE = re.compile(r"^(?:set|add)-content -encoding ascii '([^']+)' '([0-9a-f]+)'$")


def decoded_hex_file(commands: str, destination: str) -> bytes | None:
    chunks = [
        match.group(2)
        for line in commands.splitlines()
        if (match := HEX_LINE.fullmatch(line)) and match.group(1) == destination
    ]
    return bytes.fromhex("".join(chunks)) if chunks else None


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit("usage: verify_command_files.py FILE [FILE ...]")
    for name in sys.argv[1:]:
        path = Path(name)
        commands = path.read_text(encoding="ascii")
        tool.validate_generated_command_stream(commands)
        helper_bytes = decoded_hex_file(commands, tool.REMOTE_HELPER_HEX)
        helper = helper_bytes.decode("utf-8") if helper_bytes is not None else ""
        if "certutil -hashfile" not in commands and "certutil -hashfile" not in helper:
            raise AssertionError(f"{path} does not verify the remote output hash")
        if "remove-item -force 'tt.hex'" not in commands:
            raise AssertionError(f"{path} does not clean up tt.hex")
        if "decodehex 'tt.hex' 'tt.zip'" in commands and "remove-item -force 'tt.zip'" not in commands:
            raise AssertionError(f"{path} does not clean up tt.zip")
        if helper:
            for temporary in (tool.REMOTE_HELPER_HEX, tool.REMOTE_HELPER):
                if f"remove-item -force '{temporary}'" not in commands:
                    raise AssertionError(f"{path} does not clean up {temporary}")
    print(f"Verified {len(sys.argv) - 1} generated command stream(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# Building from macOS

This directory can build a native macOS typing tool locally. Windows `.exe` files should still be produced on Windows or GitHub Actions.

## Native macOS tool

Build:

```sh
./build_mac.sh
```

Output:

- `trans_type_mac`: native macOS command-line tool for the current Mac architecture.

Default behavior:

```sh
./trans_type_mac
```

By default, `trans_type_mac` reads the macOS clipboard and uses `--input-mode auto`.

In auto mode:

- ASCII text is typed as real virtual key presses. Use this path for RDP.
- Non-ASCII text is typed through Unicode `CGEvent` payloads. This is useful for local macOS apps, but some RDP clients ignore the Unicode payload and forward only the underlying keycode, which can appear remotely as repeated `a`.

For RDP, keep the input ASCII and use:

```sh
./trans_type_mac --input-mode keys
```

If shifted symbols such as `@`, `%`, `^`, `&`, or `|` arrive as unshifted keys, use the slower Windows Alt-code fallback:

```sh
./trans_type_mac --input-mode altcode
```

If both shifted keys and Alt-code fail in RDP, use the file-transfer fallback:

```sh
./trans_type_mac --windows-hex-output trans.bat
```

This types a `cmd /q /d`, `copy con tt.hex`, lowercase hex payload, Ctrl-Z, and `certutil -f -decodehex tt.hex trans.bat` sequence. It relies on remote Windows `certutil`, and creates the file without running it.

For local Unicode testing in TextEdit:

```sh
./trans_type_mac --input-mode unicode
```

Useful checks that do not type:

```sh
./trans_type_mac --dry-run
./trans_type_mac --dry-run --source file
./trans_type_mac --diagnose
./trans_type_mac --input-mode keys --dry-run
./trans_type_mac --input-mode altcode --dry-run
./trans_type_mac --input-mode unicode --dry-run
./trans_type_mac --windows-hex-output trans.txt --dry-run
```

Use `trans.txt` instead of the clipboard:

```sh
./trans_type_mac --source file
./trans_type_mac --file /path/to/input.txt
```

For actual typing, macOS must allow the terminal app or the `trans_type_mac` binary in:

```text
System Settings > Privacy & Security > Accessibility
```

You can ask macOS to show the permission prompt:

```sh
./trans_type_mac --request-accessibility --self-test
```

Visible input debug:

```sh
./trans_type_mac --debug-input
./trans_type_mac --debug-shift
```

Run this against a safe target such as Notepad inside RDP or local TextEdit. The tool prints the expected debug marker before the countdown, then types an ASCII-keys marker, a Windows Alt-code marker, and a Unicode-payload marker. The ASCII markers include common batch/shell symbols such as `@`, `%`, `^`, `&`, `|`, quotes, brackets, braces, and slashes.

If `lower=abc` appears as `LOWER=ABC` and `upper=XYZ` appears as `upper=xyz`, Caps Lock is active in the local or remote session. If letters and digits are correct but symbols differ, the issue is keyboard layout translation in the RDP/remote Windows session. Use English (US) on the remote Windows side for this tool's `--input-mode keys` path, or use `--input-mode altcode`.

Use `--debug-shift` when you suspect timing. It prints and types several `SHIFTDBG` markers with progressively slower Shift timing. A targeted manual run is:

```sh
./trans_type_mac --debug-shift --modifier-delay-ms 100 --key-hold-ms 50
```

If `--self-test` reports `Accessibility trusted: no`, enable Accessibility permission before expecting typing to work.

## Recommended path: GitHub Actions

1. Create a GitHub repository and push this directory.
2. Open the repository's **Actions** tab.
3. Run **Build Windows exes**.
4. Download the `trans_type-windows` artifact.

The artifact contains:

- `trans_type.exe`: native WinAPI C build, smallest.
- `trans_type_cpp.exe`: C++ wrapper build, same native behavior.
- `trans_type_py.exe`: Python/PyInstaller build, larger.

## Why macOS cannot directly package the Python exe

PyInstaller packages for the platform it runs on. Running PyInstaller on macOS creates a macOS binary, not a Windows `.exe`. For a Windows exe, run PyInstaller on Windows, or let GitHub Actions do it on `windows-latest`.

## Local Windows-source checks on macOS

These checks validate the Windows Python source and do not type anything:

```sh
python3 -m py_compile trans_type.py
python3 trans_type.py --dry-run
python3 trans_type.py --dry-run --ascii-only
```

The Windows tools use the Windows `SendInput`/`keybd_event` APIs. The native macOS tool uses macOS `CGEvent` APIs and requires Accessibility permission for actual typing.

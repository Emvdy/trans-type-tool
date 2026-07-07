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

By default, `trans_type_mac` reads the macOS clipboard and types it through Unicode `CGEvent` keyboard input. This is the macOS equivalent of Unicode SendInput-style typing.

Useful checks that do not type:

```sh
./trans_type_mac --dry-run
./trans_type_mac --dry-run --source file
./trans_type_mac --diagnose
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
```

Run this against a safe target such as TextEdit first. If `--self-test` reports `Accessibility trusted: no`, enable Accessibility permission before expecting typing to work.

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

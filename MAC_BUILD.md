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

By default, `trans_type_mac` reads the macOS clipboard and uses simple direct keyboard typing.

It has two transfer modes:

- `--mode simple`: directly type the source text through keyboard events.
- `--mode cmd-hex`: type a remote PowerShell + `certutil -decodehex` sequence that recreates the source text as a file on Windows.

In simple mode:

- Simple ASCII text is typed as real virtual key presses. Use this path for RDP.
- Non-ASCII text is typed through Unicode `CGEvent` payloads. This is useful for local macOS apps, but some RDP clients ignore the Unicode payload and forward only the underlying keycode, which can appear remotely as repeated `a`.

For RDP, keep the input to ASCII text without the excluded symbols, then use:

```sh
./trans_type_mac --input-mode keys
```

This simplified key mode intentionally does not handle `@`, `#`, `%`, or currency symbols. `--dry-run` reports the first unsupported character before typing starts.

For text that contains complex symbols, Chinese, or anything unreliable through direct keyboard typing, focus a remote `cmd.exe` or PowerShell prompt and use complex mode:

```sh
./trans_type_mac --mode cmd-hex --remote-output trans.txt
```

This starts PowerShell without a profile, writes the temporary hex file with `set-content` and `add-content`, then runs:

```bat
certutil -f -decodehex tt.hex trans.txt
```

It avoids `>`, `>>`, Ctrl+Z, and F6, which can be unreliable through macOS RDP.

Use `--remote-output trans.ps1` or another simple lowercase relative filename when needed.

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
./trans_type_mac --input-mode unicode --dry-run
./trans_type_mac --mode cmd-hex --dry-run
```

Use `trans.txt` instead of the clipboard:

```sh
./trans_type_mac --source file
./trans_type_mac --file /path/to/input.txt
```

Mode, input source, and speed can be combined:

```sh
./trans_type_mac --mode simple --source file --delay-ms 50 --line-delay-ms 300
./trans_type_mac --mode cmd-hex --file /path/to/input.txt --remote-output trans.ps1 --delay-ms 20 --line-delay-ms 300 --start-delay-sec 10
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

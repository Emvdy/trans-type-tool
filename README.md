# trans_type

This project provides three Windows 10 tools with the same behavior:

- `trans_type.exe`: native Win32 C/C++-style implementation. Smallest exe.
- `trans_type_cpp.exe`: C++ build wrapper for the same native implementation.
- `trans_type_py.exe`: Python implementation packaged by PyInstaller. Easier to edit, but much larger.

All three tools read `trans.txt` from the same directory as the executable and type it into the current foreground Windows window through simulated keyboard input. They are designed for Windows 10 + RDP cases where the target server has no network access and clipboard transfer is disabled.

The Windows tools do not use the clipboard, network, remote files, or any injection into the remote host. They only behave like a local keyboard typing into the focused window.

It also includes a macOS command-line tool:

- `trans_type_mac`: native Objective-C++/C++ macOS build. It reads the clipboard by default and types through macOS `CGEvent` keyboard input.

The macOS tool reads the local clipboard as an input source, but it does not paste into the target. It still types characters through simulated keyboard events.

## Build On Windows

Native WinAPI version:

```bat
build.bat
```

`build.bat` uses MSVC `cl` when available, otherwise MinGW `gcc`.

C++ wrapper version:

```bat
build_cpp.bat
```

`build_cpp.bat` compiles `trans_type.cpp`, which reuses the native WinAPI implementation and outputs `trans_type_cpp.exe`.

Python/PyInstaller version:

```bat
build_python.bat
```

`build_python.bat` installs PyInstaller if needed, then creates `trans_type_py.exe`.

All versions:

```bat
build_all.bat
```

## Build From macOS

You can edit and dry-run the Python source on macOS, but you should not expect PyInstaller on macOS to produce a Windows `.exe`. PyInstaller does not provide reliable cross-compilation from macOS to Windows.

Recommended macOS workflow:

1. Put this project in a GitHub repository.
2. Run the included GitHub Actions workflow.
3. Download the `trans_type-windows` artifact.

See `MAC_BUILD.md` for the exact macOS workflow and local dry-run checks.
See `GITHUB_BUILD.md` for step-by-step GitHub Actions instructions in Chinese.

The workflow builds all three:

- `trans_type.exe`
- `trans_type_cpp.exe`
- `trans_type_py.exe`

Expected size difference:

- Native WinAPI exe: usually tens to hundreds of KB.
- Python/PyInstaller exe: usually several MB to tens of MB because it embeds Python.

Native macOS tool:

```sh
./build_mac.sh
```

This creates `trans_type_mac` for the current Mac architecture.

## Basic Use On macOS

The macOS tool defaults to clipboard input and `--input-mode auto`:

```sh
./trans_type_mac
```

In auto mode, ASCII text is typed as real virtual key presses, which is the mode to use with RDP. If the input contains non-ASCII characters, the tool switches to Unicode `CGEvent` payloads. Some RDP clients ignore those Unicode payloads and forward only the underlying keycode; that makes the remote side see repeated `a`. For RDP, keep the clipboard/input ASCII and use:

```sh
./trans_type_mac --input-mode keys
```

If shifted symbols such as `@`, `%`, `^`, `&`, or `|` still arrive as `2`, `5`, `6`, `7`, or `\`, use the slower Windows Alt-code fallback:

```sh
./trans_type_mac --input-mode altcode
```

For local macOS apps such as TextEdit, Unicode payload mode can type non-ASCII:

```sh
./trans_type_mac --input-mode unicode
```

Useful macOS options:

```sh
./trans_type_mac --dry-run
./trans_type_mac --delay-ms 50 --line-delay-ms 300
./trans_type_mac --input-mode keys
./trans_type_mac --input-mode altcode
./trans_type_mac --input-mode unicode
./trans_type_mac --source file
./trans_type_mac --file /path/to/input.txt
./trans_type_mac --diagnose
./trans_type_mac --self-test
./trans_type_mac --debug-input
```

Use `--debug-input` against a safe editor such as Notepad/TextEdit to compare the expected marker printed in the terminal with what appears in the target. It tests common batch/shell symbols including `@`, `%`, `^`, `&`, `|`, quotes, brackets, braces, slashes, and shifted number-row symbols through both `keys` and `altcode` modes. If `lower=abc` appears as `LOWER=ABC` and `upper=XYZ` appears as `upper=xyz`, Caps Lock is active in the local or remote session. If letters and digits are correct but symbols differ, the RDP/Windows keyboard layout is not matching the US ANSI key map; set the remote Windows input layout to English (US), or use `--input-mode altcode`.

For actual typing, macOS must allow the terminal app or the `trans_type_mac` binary in **System Settings > Privacy & Security > Accessibility**. Use `--request-accessibility` if you want macOS to show the permission prompt.

## Basic Use On Windows

1. Put one of `trans_type.exe`, `trans_type_cpp.exe`, or `trans_type_py.exe` and `trans.txt` in the same directory.
2. Open the RDP session and place the cursor in the target editor, shell, or input box.
3. Run:

```bat
trans_type.exe
```

or:

```bat
trans_type_cpp.exe
```

or:

```bat
trans_type_py.exe
```

4. During the countdown, focus the target RDP window.

Useful options:

```bat
trans_type.exe --dry-run
trans_type.exe --delay-ms 50 --line-delay-ms 300
trans_type.exe --ascii-only
trans_type.exe --ascii-keys
trans_type.exe --sendinput
trans_type.exe --start-delay-sec 10
trans_type.exe --no-focus-check
trans_type.exe --self-test
trans_type_cpp.exe --self-test
trans_type_py.exe --self-test
trans_type_py.exe --diagnose
trans_type_py.exe --debug-input
```

The Python exe accepts the same options:

```bat
trans_type_py.exe --dry-run
trans_type_py.exe --delay-ms 50 --line-delay-ms 300
```

## Safety And Robustness

- `Esc` aborts typing.
- `Pause/Break` pauses typing and starts a new countdown before resuming.
- If the foreground window changes, typing pauses by default.
- The tools refuse to type into their own console.
- `--dry-run` parses `trans.txt`, reports encoding, line count, character count, non-ASCII count, and exits without typing.
- `--self-test` checks the selected input mode. By default it calls legacy `keybd_event`; use `--sendinput --self-test` to retest `SendInput`.
- Python-only `--diagnose` prints the focused target window and integrity levels without typing.
- Python-only `--debug-input` runs visible ASCII and Unicode input tests against a safe target field.
- Unsupported control characters are rejected. Tabs and newlines are allowed.
- Default maximum file size is 1 MiB. Use `--max-bytes N` only when you intentionally need more.

## Encoding

Supported inputs:

- UTF-8 with BOM
- UTF-8 without BOM
- UTF-16 LE with BOM
- UTF-16 BE with BOM
- Windows ANSI code page fallback

For scripts, ASCII text is the safest. The default mode is legacy ASCII key input because some locked-down Windows environments block `SendInput`. If `trans.txt` contains Chinese or other non-ASCII characters, use `--sendinput`; that mode depends on the Windows session allowing `SendInput`.

## Input Modes

Default mode uses legacy `keybd_event` and supports ASCII text only. It works in some environments where `SendInput` is blocked, but it depends on the local keyboard layout.

`--ascii-only` only validates that `trans.txt` contains ASCII. It does not change the input method.

`--ascii-keys` switches to virtual-key input through `SendInput` for ASCII characters. Use it only for comparison/debugging.

`--sendinput` switches to Unicode `SendInput`. Use it for non-ASCII text if the Windows session allows `SendInput`.

Enter is sent as `VK_RETURN` by default. If `--sendinput` is enabled and a target behaves better with Unicode carriage return, use:

```bat
trans_type.exe --sendinput --enter-mode unicode
```

## Exit Codes

- `0`: success
- `2`: invalid arguments
- `3`: file error
- `4`: encoding error
- `5`: content validation error
- `6`: user aborted
- `7`: keyboard input failed

## Practical Notes

Use a slower speed for high-latency RDP, KVM, or IPMI sessions:

```bat
trans_type.exe --delay-ms 80 --line-delay-ms 500
```

For PowerShell or CMD, prefer typing into a text editor first when possible, then review the content before execution. This tool does not understand or validate the script content; it only types the bytes from `trans.txt` after decoding them.

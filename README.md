# trans_type

`trans_type` types local text into a foreground window, including an RDP client,
without using remote clipboard or network transfer. It is intended for attended,
authorized administration of Windows systems.

The repository contains four implementations:

- `trans_type.exe`: native Win32 C build and the smallest Windows executable.
- `trans_type_cpp.exe`: C++ wrapper around the same native implementation.
- `trans_type_py.exe`: Python implementation packaged with PyInstaller.
- `trans_type_mac`: native Objective-C++ macOS implementation.

The Windows tools read `trans.txt` beside the executable by default. The macOS
tool reads the local clipboard by default. Clipboard input is only a local source;
the tools do not paste the clipboard into the remote session.

## Supported systems

- Windows runtime target: Windows 10 x64 or newer.
- Windows complex-mode requirements: Windows PowerShell 5.1 and `certutil`.
- `zip-hex` additionally requires PowerShell `Expand-Archive`.
- macOS runtime target: macOS 11 or newer, Intel or Apple silicon.
- The target RDP session should use an English/US-like keyboard layout and an
  English input method while typing.

The Windows GitHub workflow builds on Windows Server 2022 because GitHub does not
provide a Windows 10 hosted runner. A manual workflow run can then download the
same artifacts onto a real self-hosted Windows 10 x64 runner and repeat the full
compatibility/protocol suite. See `WINDOWS10_ACTIONS.md` for runner setup. A final
attended RDP keyboard smoke test is still required because Actions does not drive
an interactive target window.

## Transfer modes

### `simple`

Simple mode sends direct, unmodified key presses. It deliberately accepts only:

```text
a-z 0-9 space tab newline ` - = [ ] \ ; ' , . /
```

Uppercase letters, Unicode, `_`, `@`, `#`, `$`, `%`, and every other character
that needs Shift, Ctrl, Alt, or a Unicode payload are rejected before the
countdown. Use this mode only for text that fits the list above.

Tab is sent as a real Tab key. In a shell it may trigger completion or move focus
instead of inserting a tab character; use a complex mode when exact file bytes
matter.

The tools also stop or pause if Caps Lock is on or Shift/Ctrl/Alt is held. macOS
also checks Option and Command. This keeps the generated key stream independent
of modifier timing, which was unreliable in the tested RDP environment.

### `cmd-hex`

`cmd-hex` is the reliable mode for scripts, complex ASCII such as `@#$`, Chinese,
emoji, and other Unicode. It converts the selected output bytes to lowercase hex,
then types commands that reconstruct the file on remote Windows.

The generated outer stream is validated immediately before typing. It contains
only lowercase letters, digits, whitespace, and these unmodified characters:

```text
- . / \ '
```

It contains no uppercase letters and no character that requires Shift on a
US-style keyboard. Complex modes always send line endings with a physical Return
key, even if a Unicode Enter diagnostic option was supplied. A typical stream has
this shape:

```powershell
powershell -noprofile
set-content -encoding ascii 'tt.hex' '...lowercase hex...'
add-content -encoding ascii 'tt.hex' '...lowercase hex...'
certutil -f -decodehex 'tt.hex' 'trans.txt'
certutil -hashfile 'trans.txt' sha256
exit
```

Single quotes are intentional. They preserve leading zeroes in numeric-looking
hex chunks and are an unmodified key on the required keyboard layout.

### `zip-hex`

`zip-hex` first compresses the selected output bytes, then sends the zip through
the same validated lowercase hex channel. Remote Windows runs:

```powershell
certutil -f -decodehex 'tt.hex' 'tt.zip'
expand-archive -force 'tt.zip' .
certutil -hashfile 'trans.txt' sha256
```

Compression is useful for longer or repetitive content. It is normally slower
than `cmd-hex` for very small or already-compressed input. Run `--dry-run` and
compare the reported source and zip sizes before choosing it.

Neither complex mode executes the reconstructed file. The temporary `.hex` and
`.zip` files are retained for inspection and recovery. Delete them manually only
after verifying the output.

## Windows use

Put an executable beside `trans.txt`, open the RDP session, and run one of:

```bat
trans_type.exe --mode simple
trans_type_cpp.exe --mode simple
trans_type_py.exe --mode simple
```

During the five-second countdown, focus the intended target window.

Read another file or the local Windows clipboard:

```bat
trans_type.exe --mode simple --file C:\path\to\input.txt
trans_type.exe --mode simple --source clipboard
```

Use a complex mode for unrestricted text:

```bat
trans_type.exe --mode cmd-hex --file C:\path\to\input.txt --remote-output trans.txt
trans_type.exe --mode zip-hex --source clipboard --remote-output trans.txt
```

The local `--file` path may be a normal Windows path. Remote paths are more
restricted because they become part of the typed command stream: use only
lowercase letters, digits, `.`, `-`, `/`, and `\` in a relative path. Reserved
Windows device names, traversal, leading `-`, and paths longer than 240 characters
are rejected. Parent directories used by temporary or `cmd-hex` output paths must
already exist; a simple filename in the current remote directory is the safest
choice.

## macOS use

Build and run the native universal binary:

```sh
./build_mac.sh
./trans_type_mac --mode simple
```

macOS defaults to local clipboard input. Use `trans.txt` or another file with:

```sh
./trans_type_mac --mode simple --source file
./trans_type_mac --mode cmd-hex --file /path/to/input.txt --remote-output trans.txt
./trans_type_mac --mode zip-hex --file /path/to/input.txt --remote-output trans.txt
```

Simple mode never automatically switches to Unicode. `--input-mode unicode` is
kept only as an explicit diagnostic transport and does not bypass simple-mode
character validation. Use `cmd-hex` or `zip-hex` for real Unicode transfer.

Actual macOS typing requires Accessibility permission for the terminal or binary:

```text
System Settings > Privacy & Security > Accessibility
```

Request the prompt and run a non-visible key test with:

```sh
./trans_type_mac --request-accessibility --self-test
```

## Output encoding

Complex modes support:

- `--output-encoding utf8`: decode the input text and recreate UTF-8 without a
  BOM. This is the default.
- `--output-encoding utf8-bom`: recreate UTF-8 with a BOM.
- `--output-encoding preserve`: recreate the original file bytes exactly. This
  requires file input; clipboard text has no original byte encoding.

`preserve` is not an arbitrary binary mode. The source must still decode as one
of the supported text encodings so content validation can run.

Supported file decoding includes UTF-8 and UTF-16 LE/BE with BOM. Windows uses
the active ANSI code page as its final fallback; macOS uses Windows-1252.

Examples:

```bat
trans_type.exe --mode cmd-hex --output-encoding utf8-bom
trans_type.exe --mode zip-hex --file C:\path\to\input.txt --output-encoding preserve
```

Each complex transfer prints the expected local SHA-256. The generated remote
command prints the reconstructed file's SHA-256 with `certutil`. Compare the two
values; the program cannot read remote console output back through the keyboard
channel automatically.

## Speed and command auditing

```bat
trans_type.exe --delay-ms 50 --line-delay-ms 300 --start-delay-sec 10
trans_type.exe --mode cmd-hex --hex-chunk-bytes 240
```

- `--delay-ms`: delay after each typed character, from 0 to 5000 ms.
- `--line-delay-ms`: delay after each Enter, from 0 to 5000 ms.
- `--start-delay-sec`: focus countdown, from 0 to 3600 seconds.
- `--hex-chunk-bytes`: bytes encoded per generated hex line, from 1 to 2048.

Larger chunks reduce command overhead but create longer RDP input lines. Start at
the default 240. For unstable or high-latency sessions, increase both typing
delays before increasing chunk size.

Inspect the exact complex stream without typing:

```bat
trans_type.exe --mode cmd-hex --dry-run --commands-out commands.txt
trans_type.exe --mode zip-hex --dry-run --commands-out commands.txt
```

The exporter writes the same stream that the program validates and would type.

## Build

On Windows with MSVC or MinGW:

```bat
build.bat
build_cpp.bat
build_python.bat
```

Build all three Windows executables:

```bat
build_all.bat
```

`build_python.bat` uses the pinned versions in `requirements-build.txt` and
creates a PyInstaller one-file executable. PyInstaller embeds Python, so this
artifact is much larger and can receive more antivirus false positives than the
native C build. UPX and code obfuscation are not used.

PyInstaller is not a Windows cross-compiler. Running it on macOS produces a
macOS application, not a Windows `.exe`. Build Windows artifacts on Windows or
use `.github/workflows/build-windows.yml`.

The macOS build defaults to a universal `arm64 + x86_64` binary with a macOS 11
deployment target:

```sh
./build_mac.sh
```

See `MAC_BUILD.md`, `GITHUB_BUILD.md`, and `WINDOWS10_ACTIONS.md` for
platform-specific instructions.

## Diagnostics and controls

Windows:

```bat
trans_type.exe --self-test
trans_type_cpp.exe --self-test
trans_type_py.exe --self-test
trans_type_py.exe --diagnose
trans_type_py.exe --debug-input
```

- `--self-test` uses F24 on Windows and F20 on macOS, not Shift.
- Windows `--diagnose` reports foreground-window and integrity information.
- Windows `--debug-input` visibly compares legacy keys, SendInput virtual keys,
  and Unicode SendInput. Run it only in a disposable text field.
- `Esc` aborts. Windows `Pause/Break` pauses and requires a new countdown.
- Windows pauses when focus changes; macOS aborts when the foreground app/window
  changes. `--no-focus-check` disables this protection and should be used only
  for a target whose window identity cannot remain stable.

Normal medium-integrity RDP operation does not require administrator rights. If
the target application itself is elevated, Windows may block synthetic input
from a non-elevated tool. Reopen the target normally instead of bypassing the
integrity boundary.

Before typing, turn Caps Lock off, release all modifiers, select an English input
method, and verify that local and remote keyboard layouts match.

## Operational security

This project does not use the network, inject into another process, elevate,
hide execution, establish persistence, or bypass endpoint policy. Complex modes
do invoke visible Windows built-ins, so enterprise controls such as AppLocker,
WDAC, or EDR may block or alert on them. Do not bypass those controls; obtain an
allowlist or approved transfer method from the system owner.

For internal distribution, retain source, record release SHA-256 values, and use
an organizational Authenticode/code-signing certificate when available.

## Exit codes

- `0`: success
- `2`: invalid arguments
- `3`: file error
- `4`: encoding error
- `5`: content validation error
- `6`: user aborted
- `7`: keyboard input failed

# trans_type

`trans_type` types local text or a file-reconstruction command stream into a
foreground window, including an RDP client, without using remote clipboard or
network transfer. It can copy one arbitrary file through `cmd-hex`/`zip-hex`, or
one directory tree through `zip-hex`. It is intended for attended, authorized
administration of Windows systems.

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

## Sources and modes

`--source` is the canonical source selector:

```text
--source clipboard          local clipboard text
--source file               trans.txt beside the executable/binary
--source <local-path>       a local regular file, or a directory in zip-hex
```

`--file` has been removed. Use `--source <local-path>` for both text and binary
file sources.

The supported matrix is:

| Mode | Clipboard text | Text file | Arbitrary regular file | Directory |
|---|---:|---:|---:|---:|
| `simple` | yes | yes | no | no |
| `cmd-hex` | yes | yes | yes, with `preserve` | no |
| `zip-hex` | yes | yes | yes, with `preserve` | yes |

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
emoji, other Unicode, and one arbitrary regular file. It converts the selected
output bytes to lowercase hex, then types commands that reconstruct the file on
remote Windows. Use `--output-encoding preserve` for a byte-for-byte binary copy.

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
certutil -f -decodehex 'tt.hex' '.\trans.txt'
certutil -hashfile '.\trans.txt' sha256
remove-item -force 'tt.hex'
exit
```

Single quotes are intentional. They preserve leading zeroes in numeric-looking
hex chunks and are an unmodified key on the required keyboard layout.

### `zip-hex`

For a regular file or text source, `zip-hex` first compresses the selected output
bytes, then sends the zip through the same validated lowercase hex channel.
Remote Windows runs:

```powershell
certutil -f -decodehex 'tt.hex' 'tt.zip'
expand-archive -force 'tt.zip' .
certutil -hashfile '.\trans.txt' sha256
remove-item -force 'tt.hex'
remove-item -force 'tt.zip'
```

Compression is useful for longer or repetitive content. It is normally slower
than `cmd-hex` for very small or already-compressed input. Run `--dry-run` and
compare the reported source and zip sizes before choosing it.

For a directory source, the output name is the destination directory, and the
archive contains the source directory's contents rather than the local root
directory itself:

```powershell
certutil -hashfile 'tt.zip' sha256
expand-archive -force 'tt.zip' '.\copied-dir'
```

The displayed expected hash is the ZIP archive hash. Matching it proves the hex
transport arrived intact; `Expand-Archive` also checks each ZIP entry's CRC while
extracting. Extraction overwrites/merges files in an existing destination but
does not delete unrelated files already there, so this is a copy operation, not
an exact directory mirror.

Directory transfer rejects symbolic links, junctions, reparse points, and special
files. It allows at most 10,000 entries and limits the sum of regular-file bytes
with `--max-bytes`. Directory entry names are ZIP metadata and may contain
uppercase or Unicode; they are hex-encoded and never appear literally in the
typed command stream. Names must still be valid on the target Windows filesystem.

Neither complex mode executes the reconstructed file. The fixed intermediate
files are `tt.hex` and, for `zip-hex`, `tt.zip`. A target path that cannot be
typed without Shift additionally uses an internal `tt.cmd.hex`/`tt.cmd` helper;
`cmd-hex` also stages data as `tt.out`. The helper only creates directories,
moves/extracts the reconstructed data, and prints a hash. It never executes the
user file. The generated stream deletes all intermediates after reconstruction
or extraction. If typing is aborted before cleanup, remove any partial `tt.*`
intermediates before retrying.

## Remote output and path

`--remote-output TARGET` combines the former output-name and parent-path
options. The old separate parent-path option has been removed. The default
target depends on the source:

```text
clipboard                 .\trans.txt
--source file             .\trans.txt
--source <local-path>     .\<local-basename>
```

`TARGET` may be a new name, a relative path, a current-drive rooted path, a
drive-letter absolute path, or a UNC path. `/` and `\` are both accepted and
normalized to Windows separators. A target ending in `/` or `\`, or exactly `.`
or `..`, is treated as a directory and receives the local source basename:

```text
option omitted                         .\source.bin
--remote-output renamed.bin            .\renamed.bin
--remote-output ../                    ..\source.bin
--remote-output ../renamed.bin         ..\renamed.bin
--remote-output c:/work/drop/          c:\work\drop\source.bin
--remote-output c:/work/drop/file.bin  c:\work\drop\file.bin
```

Missing parent directories are created. Invalid Windows components, reserved
device names, trailing dots/spaces, malformed drive/UNC paths, and the fixed
temporary names are rejected before typing. `--remote-output` applies only to
`cmd-hex` and `zip-hex`; `simple` types text directly and creates no file.

Shift-free lowercase targets use the shorter direct protocol. Targets containing
uppercase, spaces, `_`, `:`, Unicode, or other modifier-dependent characters use
the hex-encoded internal helper. The literal complex path therefore never
appears in the simulated outer key stream, which remains lowercase and
Shift-free. The helper adds typing overhead, so a lowercase relative target is
faster when the final location/name is flexible.

## Windows use

Put an executable beside `trans.txt`, open the RDP session, and run one of:

```bat
trans_type.exe --mode simple
trans_type_cpp.exe --mode simple
trans_type_py.exe --mode simple
```

During the five-second countdown, focus the intended target window.

Read another text file or the local Windows clipboard:

```bat
trans_type.exe --mode simple --source C:\path\to\input.txt
trans_type.exe --mode simple --source clipboard
```

Use a complex mode for unrestricted text:

```bat
trans_type.exe --mode cmd-hex --source C:\path\to\input.txt
trans_type.exe --mode zip-hex --source clipboard
```

Copy an arbitrary file byte for byte, or copy a directory tree:

```bat
trans_type.exe --mode cmd-hex --source C:\path\to\tool.exe --output-encoding preserve
trans_type.exe --mode zip-hex --source C:\path\to\data
trans_type.exe --mode cmd-hex --source C:\path\to\input.txt --remote-output ../
trans_type.exe --mode zip-hex --source C:\path\to\tool.exe --output-encoding preserve --remote-output C:/Drop Folder/tool.exe
```

The local `--source` path may be a normal Windows path and is never typed into the
RDP session. A shift-free target appears in the generated command stream;
otherwise only its lowercase hex-encoded helper representation is typed.

## macOS use

Build and run the native universal binary:

```sh
./build_mac.sh
./trans_type_mac --mode simple
```

macOS defaults to local clipboard input. Use `trans.txt` or another file with:

```sh
./trans_type_mac --mode simple --source file
./trans_type_mac --mode cmd-hex --source /path/to/input.txt
./trans_type_mac --mode zip-hex --source /path/to/input.txt
./trans_type_mac --mode zip-hex --source /path/to/data
./trans_type_mac --mode cmd-hex --source /path/to/input.txt --remote-output '../'
./trans_type_mac --mode zip-hex --source /path/to/data --remote-output 'C:/Drop Folder/'
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

`preserve` is the arbitrary regular-file mode. It bypasses text decoding and
copies every byte, including NUL bytes and data that is not valid text. It is
accepted only by `cmd-hex` and `zip-hex`. Directory entries are always preserved
as raw bytes by `zip-hex`, so their transfer does not use text output encoding.

Supported file decoding includes UTF-8 and UTF-16 LE/BE with BOM. Windows uses
the active ANSI code page as its final fallback; macOS uses Windows-1252.

Examples:

```bat
trans_type.exe --mode cmd-hex --output-encoding utf8-bom
trans_type.exe --mode zip-hex --source C:\path\to\input.bin --output-encoding preserve --remote-output input.bin
```

Each regular-file complex transfer prints the expected output SHA-256, and the
remote command hashes the reconstructed file with `certutil`. Directory mode
instead prints and remotely checks the transferred ZIP SHA-256 before extraction.
Compare the two values; the program cannot read remote console output back through
the keyboard channel automatically.

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
- Windows/Python: `Esc` aborts; there is no global pause hotkey.
- macOS: the first physical `Esc` is consumed locally and requests a pause after
  the current line and Return are complete. While paused, Space resumes and Esc
  ends the transfer. `Ctrl-C` ends it when the terminal is focused or receives
  SIGINT.
- macOS marks its synthetic events so command spaces bypass control handling;
  physical Esc/Space control events are consumed locally and do not enter RDP.
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

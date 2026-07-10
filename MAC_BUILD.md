# Building and testing on macOS

macOS can build the native Mac tool and validate the shared protocol. It cannot
directly produce the Windows PyInstaller executable; Windows artifacts are built
on Windows or GitHub Actions.

## Native universal build

Requirements:

- Xcode Command Line Tools with `clang++`
- macOS SDK containing Intel and Apple-silicon targets
- `/usr/bin/zip` for `zip-hex`

Build:

```sh
./build_mac.sh
```

Default output:

- file: `trans_type_mac`
- architectures: `arm64` and `x86_64`
- minimum runtime: macOS 11.0

Verify it:

```sh
lipo -info trans_type_mac
xcrun vtool -show-build trans_type_mac
```

Override architectures or deployment target when needed:

```sh
TRANS_TYPE_MAC_ARCHS="arm64" ./build_mac.sh
MACOSX_DEPLOYMENT_TARGET=12.0 ./build_mac.sh
```

Sign with an installed identity:

```sh
CODESIGN_IDENTITY="Developer ID Application: Example" ./build_mac.sh
codesign --verify --deep --strict --verbose=2 trans_type_mac
```

Signing does not replace Apple notarization for public distribution.

## macOS behavior

The default source is the local clipboard. The default mode is strict `simple`:

```sh
./trans_type_mac
```

Simple mode types only lowercase letters, digits, tabs/newlines, spaces, and
unmodified US-keyboard punctuation. It rejects uppercase, Unicode, `@#$`, and
all Shift-dependent characters before requesting Accessibility permission or
starting the countdown.

Use file input:

```sh
./trans_type_mac --source file
./trans_type_mac --file /path/to/input.txt
```

Use a complex mode for scripts, symbols, or Unicode. Focus a remote `cmd.exe` or
PowerShell prompt during the countdown:

```sh
./trans_type_mac --mode cmd-hex --file /path/to/input.txt --remote-output trans.txt
./trans_type_mac --mode zip-hex --file /path/to/input.txt --remote-output trans.txt
```

Both modes type a validated stream containing no uppercase and no Shift-required
characters. `cmd-hex` sends source bytes directly as hex. `zip-hex` uses the
macOS system `/usr/bin/zip` first and is useful only when compression saves enough
typing.

Simple mode does not automatically switch to Unicode. `--input-mode unicode` is
retained for explicit diagnostics and does not bypass strict simple validation.

## Encoding and auditing

```sh
./trans_type_mac --mode cmd-hex --output-encoding utf8
./trans_type_mac --mode cmd-hex --output-encoding utf8-bom
./trans_type_mac --mode zip-hex --file /path/to/input.txt --output-encoding preserve
```

`preserve` requires file input and recreates the original decoded file bytes.

Export and inspect the exact generated stream without typing:

```sh
./trans_type_mac --mode cmd-hex --source file --dry-run --commands-out cmd.commands
./trans_type_mac --mode zip-hex --source file --dry-run --commands-out zip.commands
python3 tests/verify_command_files.py cmd.commands zip.commands
```

The tool displays the expected source SHA-256. After remote reconstruction,
compare it with the SHA-256 printed by remote `certutil`.

## Accessibility and focus

Actual typing requires Accessibility permission for the terminal or binary:

```text
System Settings > Privacy & Security > Accessibility
```

Request the prompt and test a harmless F20 event:

```sh
./trans_type_mac --request-accessibility --self-test
```

Visible diagnostics:

```sh
./trans_type_mac --debug-input
```

Run visible diagnostics only against a disposable TextEdit document or another
safe field. The debug command tests both virtual keys and Unicode event payloads;
it is not a transfer mode.

The program records both foreground application PID and window ID. It aborts if
either changes while typing. Before starting, turn Caps Lock off, release Shift,
Control, Option, and Command, select an English input method, and align the local
and remote keyboard layouts.

## Local verification

Run the protocol unit tests, compile, and validate all native payload variants:

```sh
python3 -m unittest discover -s tests -v
./build_mac.sh
python3 tests/verify_macos_binary.py ./trans_type_mac
git diff --check
```

The binary verifier checks:

- strict simple acceptance and rejection
- lowercase/no-Shift generated command streams
- `cmd-hex` payload reconstruction
- `zip-hex` archive contents
- UTF-8, UTF-8 BOM, and byte-preserving output

These dry-run tests do not post keyboard events.

## Windows builds from macOS

PyInstaller packages for its host operating system. Running PyInstaller on macOS
does not create a supported Windows `.exe`. Use the repository's Windows workflow:

```text
.github/workflows/build-windows.yml
```

It runs on Windows Server 2022 x64, builds all three Windows executables, verifies
their PE architecture, and executes the generated protocols using Windows
PowerShell 5.1 and `certutil`. See `GITHUB_BUILD.md` for the complete procedure.

The hosted runner validates Windows compatibility but is not an actual Windows
10 RDP client. Perform the final attended simple/cmd-hex/zip-hex smoke test on the
intended Windows 10 machine before operational deployment.

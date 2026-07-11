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
./trans_type_mac --source /path/to/input.txt
```

`--source` accepts `clipboard`, `file` (the adjacent `trans.txt`), or a local
path. `--file` has been removed.

Use a complex mode for scripts, symbols, or Unicode. Focus a remote `cmd.exe` or
PowerShell prompt during the countdown:

```sh
./trans_type_mac --mode cmd-hex --source /path/to/input.txt
./trans_type_mac --mode zip-hex --source /path/to/input.txt
./trans_type_mac --mode zip-hex --source /path/to/folder
```

Both modes type a validated stream containing no uppercase and no Shift-required
characters. `cmd-hex` sends source bytes directly as hex. `zip-hex` uses the
macOS system `/usr/bin/zip` first and is useful only when compression saves enough
typing.

Copy arbitrary regular-file bytes with `--output-encoding preserve`. Directory
sources are accepted only by `zip-hex`; their contents are extracted into the
remote directory named by the source basename by default. Existing destination files are
overwritten/merged, but unrelated files are not deleted. Symbolic links and
special files are rejected.

Simple mode does not automatically switch to Unicode. `--input-mode unicode` is
retained for explicit diagnostics and does not bypass strict simple validation.

## Remote output and path

Clipboard input defaults to remote `.\trans.txt`. A path source defaults to
`.\<local-basename>`. `--remote-output TARGET` selects both the remote name and
location; the former separate path option has been removed.

Both slash styles are accepted. A target ending in `/` or `\`, or exactly `.`
or `..`, is a directory target and receives the source basename:

```sh
./trans_type_mac --mode cmd-hex --source /path/to/input.txt \
  --remote-output '../'
./trans_type_mac --mode zip-hex --source /path/to/input.bin \
  --output-encoding preserve --remote-output 'C:/Drop Folder/output.bin'
```

The accepted forms include a name, relative path, current-drive rooted path,
drive-letter absolute path, and UNC path. Missing parents are created. Malformed
Windows paths, reserved device names, trailing dots/spaces, and fixed temporary
names are rejected.

A lowercase target containing only unmodified keys uses the shorter direct
protocol. Uppercase, spaces, `_`, `:`, Unicode, and other modifier-dependent
characters select an internal hex-encoded `tt.cmd` helper. The literal path does
not appear in the simulated outer stream, so that stream remains lowercase and
Shift-free. Helper transfers also use `tt.cmd.hex`; `cmd-hex` stages data in
`tt.out`. All fixed intermediates are deleted after success. If typing is
aborted before cleanup, remove partial `tt.*` files before retrying.

`--remote-output` is valid only in `cmd-hex` and `zip-hex`. Simple mode creates
no remote file.

## Encoding and auditing

```sh
./trans_type_mac --mode cmd-hex --output-encoding utf8
./trans_type_mac --mode cmd-hex --output-encoding utf8-bom
./trans_type_mac --mode zip-hex --source /path/to/input.bin --output-encoding preserve --remote-output input.bin
```

`preserve` requires a regular-file source in `cmd-hex` or `zip-hex`, bypasses
text decoding, and recreates arbitrary file bytes exactly. A directory source is
always archived byte for byte and does not use text output encoding.

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

Press Esc to abort. Tap physical Space to pause and tap it again to resume. The
Space reaches the target first, then the tool sends one Delete/Backspace after
key release to remove it. Do not hold Space long enough to trigger key repeat.

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
- arbitrary binary files and nested directory ZIP payloads
- source-basename defaults, merged target paths, helper isolation, and temporary-file cleanup

These dry-run tests do not post keyboard events.

## Windows builds from macOS

PyInstaller packages for its host operating system. Running PyInstaller on macOS
does not create a supported Windows `.exe`. Use the repository's Windows workflow:

```text
.github/workflows/build-windows.yml
```

It first runs on Windows Server 2022 x64, builds all three Windows executables,
verifies their PE architecture, and executes the generated protocols using
Windows PowerShell 5.1 and `certutil`. A manual run can repeat the suite on a real
self-hosted Windows 10 x64 runner. See `GITHUB_BUILD.md` and
`WINDOWS10_ACTIONS.md` for the complete procedure.

The hosted runner validates Windows compatibility but is not an actual Windows
10 RDP client. Perform the final attended simple/cmd-hex/zip-hex smoke test on the
intended Windows 10 machine before operational deployment.

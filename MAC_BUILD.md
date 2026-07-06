# Building Windows exe files from macOS

This directory can be edited and tested from macOS, but Windows `.exe` files should be produced on Windows.

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

## Local macOS checks

These checks do not type anything:

```sh
python3 -m py_compile trans_type.py
python3 trans_type.py --dry-run
python3 trans_type.py --dry-run --ascii-only
```

Actual keyboard simulation is Windows-only because it uses the Windows `SendInput` API.

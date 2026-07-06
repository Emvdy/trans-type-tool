@echo off
setlocal

cd /d "%~dp0"

set "PYTHON=python"
where python >nul 2>nul
if not errorlevel 1 goto have_python

where py >nul 2>nul
if errorlevel 1 goto no_python
set "PYTHON=py -3"

:have_python
%PYTHON% -m PyInstaller --version >nul 2>nul
if not errorlevel 1 goto build_pyinstaller

echo PyInstaller is not installed. Installing it now...
%PYTHON% -m pip install --upgrade -r requirements-build.txt
if errorlevel 1 exit /b %ERRORLEVEL%

:build_pyinstaller
echo Building Python one-file exe...
%PYTHON% -m PyInstaller --clean --onefile --console --name trans_type_py trans_type.py
if errorlevel 1 exit /b %ERRORLEVEL%

copy /Y dist\trans_type_py.exe trans_type_py.exe >nul
if errorlevel 1 exit /b %ERRORLEVEL%

echo Built trans_type_py.exe
exit /b 0

:no_python
echo Python was not found. Install Python 3.10+ for Windows, then run this script again.
exit /b 1

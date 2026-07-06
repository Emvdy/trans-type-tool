@echo off
setlocal

cd /d "%~dp0"

call build.bat
if errorlevel 1 exit /b %ERRORLEVEL%

call build_cpp.bat
if errorlevel 1 exit /b %ERRORLEVEL%

call build_python.bat
if errorlevel 1 exit /b %ERRORLEVEL%

echo.
echo Built all Windows executables:
if exist trans_type.exe echo   trans_type.exe
if exist trans_type_cpp.exe echo   trans_type_cpp.exe
if exist trans_type_py.exe echo   trans_type_py.exe
exit /b 0

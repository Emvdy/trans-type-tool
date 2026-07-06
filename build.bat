@echo off
setlocal

cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto build_msvc

where gcc >nul 2>nul
if not errorlevel 1 goto build_gcc

echo No supported Windows C compiler was found.
echo Install Visual Studio Build Tools or MinGW-w64, then run build.bat again.
exit /b 1

:build_msvc
echo Building with MSVC...
cl /nologo /O2 /MT /W4 /D_CRT_SECURE_NO_WARNINGS trans_type.c user32.lib
exit /b %ERRORLEVEL%

:build_gcc
echo Building with GCC/MinGW...
gcc -Os -s -Wall -Wextra -o trans_type.exe trans_type.c -luser32
exit /b %ERRORLEVEL%

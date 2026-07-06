@echo off
setlocal

cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto build_msvc

where g++ >nul 2>nul
if not errorlevel 1 goto build_gcc

echo No supported Windows C++ compiler was found.
echo Install Visual Studio Build Tools or MinGW-w64, then run build_cpp.bat again.
exit /b 1

:build_msvc
echo Building C++ wrapper with MSVC...
cl /nologo /TP /O2 /MT /W4 /D_CRT_SECURE_NO_WARNINGS /Fe:trans_type_cpp.exe /Fo:trans_type_cpp.obj trans_type.cpp user32.lib
exit /b %ERRORLEVEL%

:build_gcc
echo Building C++ wrapper with GCC/MinGW...
g++ -Os -s -Wall -Wextra -o trans_type_cpp.exe trans_type.cpp -luser32
exit /b %ERRORLEVEL%

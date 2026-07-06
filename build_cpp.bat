@echo off
setlocal

cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto build_msvc

where gcc >nul 2>nul
if errorlevel 1 goto no_compiler

where g++ >nul 2>nul
if errorlevel 1 goto no_compiler
goto build_gcc

:build_msvc
echo Building C implementation object for C++ wrapper with MSVC...
cl /nologo /O2 /MT /W4 /D_CRT_SECURE_NO_WARNINGS /DTRANS_TYPE_CPP_BUILD /c /Fotrans_type_cpp_native.obj trans_type.c
if errorlevel 1 exit /b %ERRORLEVEL%

echo Linking C++ wrapper with MSVC...
cl /nologo /TP /O2 /MT /W4 /D_CRT_SECURE_NO_WARNINGS /Fetrans_type_cpp.exe trans_type.cpp trans_type_cpp_native.obj user32.lib
exit /b %ERRORLEVEL%

:build_gcc
echo Building C implementation object for C++ wrapper with GCC/MinGW...
gcc -Os -Wall -Wextra -DTRANS_TYPE_CPP_BUILD -c -o trans_type_cpp_native.o trans_type.c
if errorlevel 1 exit /b %ERRORLEVEL%

echo Linking C++ wrapper with GCC/MinGW...
g++ -Os -s -Wall -Wextra -o trans_type_cpp.exe trans_type.cpp trans_type_cpp_native.o -luser32
exit /b %ERRORLEVEL%

:no_compiler
echo No supported Windows C/C++ compiler pair was found.
echo Install Visual Studio Build Tools or MinGW-w64, then run build_cpp.bat again.
exit /b 1

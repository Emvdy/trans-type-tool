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
echo Building C++ launcher with MSVC...
cl /nologo /EHsc /O2 /MT /W4 /D_CRT_SECURE_NO_WARNINGS /Fetrans_type_cpp.exe trans_type.cpp
if not errorlevel 1 exit /b 0
goto fallback_native

:build_gcc
echo Building C++ launcher with GCC/MinGW...
g++ -Os -s -Wall -Wextra -o trans_type_cpp.exe trans_type.cpp
if not errorlevel 1 exit /b 0
goto fallback_native

:fallback_native
echo C++ launcher build failed. Falling back to a native WinAPI alias.
if exist trans_type.exe (
    copy /Y trans_type.exe trans_type_cpp.exe >nul
    if not errorlevel 1 (
        echo Built trans_type_cpp.exe as a native alias for trans_type.exe.
        exit /b 0
    )
)

echo Fallback failed because trans_type.exe is missing or could not be copied.
exit /b 1

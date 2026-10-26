@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto msvc

where gcc >nul 2>nul
if not errorlevel 1 goto mingw

echo [ERROR] Neither MSVC (cl) nor MinGW-w64 (gcc) was found in PATH.
echo Open a "x64 Native Tools Command Prompt for VS" or install MSYS2/MinGW.
exit /b 1

:msvc
echo [*] Building with MSVC...
cl /nologo /std:c11 /O2 /W4 /utf-8 /MT /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE src\main.c src\util.c src\json.c src\hash.c src\http.c src\inflate.c src\zip.c src\version.c src\install.c src\remote.c src\launch.c /Fe:mclc.exe /link winhttp.lib shell32.lib
if errorlevel 1 exit /b 1
echo [OK] mclc.exe
exit /b 0

:mingw
echo [*] Building with MinGW-w64...
gcc -std=gnu11 -O2 -Wall -Wextra -Wpedantic -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN src\main.c src\util.c src\json.c src\hash.c src\http.c src\inflate.c src\zip.c src\version.c src\install.c src\remote.c src\launch.c -static -static-libgcc -lwinhttp -lshell32 -o mclc.exe
if errorlevel 1 exit /b 1
echo [OK] mclc.exe
exit /b 0

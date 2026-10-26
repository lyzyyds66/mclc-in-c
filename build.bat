@echo off
setlocal
cd /d "%~dp0"

set CORE=src\util.c src\json.c src\hash.c src\http.c src\inflate.c src\zip.c src\version.c src\install.c src\remote.c src\launch.c src\progress.c

where cl >nul 2>nul
if not errorlevel 1 goto msvc

where gcc >nul 2>nul
if not errorlevel 1 goto mingw

echo [ERROR] Neither MSVC (cl) nor MinGW-w64 (gcc) was found in PATH.
echo Open a "x64 Native Tools Command Prompt for VS" or install MSYS2/MinGW.
exit /b 1

:msvc
echo [*] Building GUI with MSVC...
rc /nologo /I src /fo mclc.res src\mclc.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /O2 /W4 /utf-8 /MT /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE src\gui.c %CORE% mclc.res /Fe:mclc_gui.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO winhttp.lib shell32.lib comdlg32.lib ole32.lib user32.lib gdi32.lib comctl32.lib
if errorlevel 1 exit /b 1
echo [OK] mclc_gui.exe
exit /b 0

:mingw
echo [*] Building GUI with MinGW-w64...
windres -I src src\mclc.rc -O coff -o mclc.res.o
if errorlevel 1 exit /b 1
gcc -std=gnu11 -O2 -Wall -Wextra -Wpedantic -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN src\gui.c %CORE% mclc.res.o -mwindows -static -static-libgcc -lwinhttp -lshell32 -lcomdlg32 -lole32 -luser32 -lgdi32 -lcomctl32 -o mclc_gui.exe
if errorlevel 1 exit /b 1
echo [OK] mclc_gui.exe
exit /b 0

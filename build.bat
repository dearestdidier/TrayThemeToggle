@echo off
rem ===========================================================================
rem  Build script: MinGW-w64 g++ + windres
rem  Output: build\TrayThemeToggle.exe  (single file, no runtime deps)
rem
rem  Note: -specs=src/no-default-manifest.specs is required.
rem        MinGW auto-links a default-manifest.o, and merging it with our own
rem        manifest fails with "multiple non-default manifests". The specs file
rem        removes that automatic endfile entry.
rem ===========================================================================
setlocal

cd /d "%~dp0"

set "CXX=g++.exe"
set "WINDRES=windres.exe"

where %CXX% >nul 2>nul
if errorlevel 1 (
    echo [ERROR] g++.exe not found. Add MinGW-w64 bin to PATH.
    exit /b 1
)
where %WINDRES% >nul 2>nul
if errorlevel 1 (
    echo [ERROR] windres.exe not found.
    exit /b 1
)

if not exist build mkdir build

echo [1/3] Compiling resources...
%WINDRES% -I src -I res -O coff -F pe-x86-64 src\app.rc -o build\app.res.o
if errorlevel 1 (
    echo [ERROR] resource compilation failed
    exit /b 1
)

echo [2/3] Compiling main.cpp...
%CXX% -std=c++17 -O2 -municode -mwindows -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Wall -Wextra -Wno-unused-parameter -specs=src/no-default-manifest.specs src\main.cpp build\app.res.o -o build\TrayThemeToggle.exe -luser32 -lshell32 -ladvapi32 -lgdi32 -lole32 -ldwmapi -static -static-libgcc -static-libstdc++ -s
if errorlevel 1 (
    echo [ERROR] compilation failed
    exit /b 1
)

echo [3/3] Done.
echo.
for %%F in (build\TrayThemeToggle.exe) do echo    output: %%~fF  ^(%%~zF bytes^)
echo.
echo    Double-click to run. A tray icon will appear.
echo    Run "build\TrayThemeToggle.exe --diagnose" to write diagnose.log.
endlocal

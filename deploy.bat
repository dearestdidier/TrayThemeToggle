@echo off
rem ===========================================================================
rem  把编译好的 exe 部署到一个正常位置（默认 %LOCALAPPDATA%\TrayThemeToggle）
rem
rem  为什么要复制？
rem    本工程位于 DSH 沙箱工作区内，沙箱会给工作区里的文件打上
rem    "Low Mandatory Level" 完整性标签。带这个标签的 exe 被双击时，
rem    Windows 会自动把进程降到 Low 完整性级别，于是：
rem      - Shell_NotifyIconW 被 UIPI 拒绝  -> 托盘没有图标
rem      - 写 HKCU 主题设置被拒绝          -> 切换无效
rem    看起来就是「打不开 / 没反应」。
rem    复制到工作区外（Temp、LOCALAPPDATA、桌面等）后标签不再存在，
rem    进程以正常的 Medium 级别运行，一切正常。
rem ===========================================================================
setlocal

cd /d "%~dp0"

set "SRC=build\TrayThemeToggle.exe"
if not exist "%SRC%" (
    echo [ERROR] %SRC% not found. Run build.bat first.
    exit /b 1
)

set "DESTDIR=%LOCALAPPDATA%\TrayThemeToggle"
if not exist "%DESTDIR%" mkdir "%DESTDIR%"

copy /y "%SRC%" "%DESTDIR%\TrayThemeToggle.exe" >nul
if errorlevel 1 (
    echo [ERROR] copy failed
    exit /b 1
)

rem Strip the sandbox "Low Mandatory Level" integrity label from the copy.
rem A Low-labeled exe runs as a Low-integrity process; UIPI then blocks the
rem tray icon (Shell_NotifyIconW) and HKCU theme writes -> "can't open".
rem Setting Medium on the file (and its folder) fixes it from the source up.
icacls "%DESTDIR%" /setintegritylevel M >nul 2>&1
icacls "%DESTDIR%\TrayThemeToggle.exe" /setintegritylevel M >nul 2>&1

echo.
echo   Deployed to: %DESTDIR%\TrayThemeToggle.exe
echo.
echo   Double-click that file to start.
echo   (Do NOT run the one under build\ - the sandbox low-integrity label
echo    on that copy blocks tray registration and registry writes.)
echo.
echo   Optional: create a desktop shortcut
echo     right-click the deployed exe - Send to - Desktop (create shortcut)
echo.

choice /c YN /n /m "Launch it now? [Y/N] "
if errorlevel 2 goto :done
start "" "%DESTDIR%\TrayThemeToggle.exe"
echo   launched.

:done
endlocal

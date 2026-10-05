@echo off
setlocal
set "ROOT=%~dp0"
set "DISK=%ROOT%CiukiOS.img"

if not exist "%DISK%" (
    echo.
    echo ERROR: CiukiOS.img not found in "%ROOT%"!
    echo Make sure the entire ZIP archive has been extracted into a normal folder.
    echo.
    pause
    exit /b 1
)

echo ========================================================
echo                  CiukiOS Portable Launcher
echo ========================================================
echo  [1] Failsafe Standard VGA  (Universal compatibility)
echo  [2] VirtIO-GPU             (Direct 2D presentation)
echo ========================================================
echo.
choice /C 12 /N /T 5 /D 1 /M "Select display mode [1=VGA, 2=VirtIO] (default 1 in 5s): "
if errorlevel 2 goto run_virtio
goto run_vga

:run_virtio
call "%ROOT%Start-CiukiOS-VirtIO.cmd"
exit /b %ERRORLEVEL%

:run_vga
call "%ROOT%Start-CiukiOS-VGA.cmd"
exit /b %ERRORLEVEL%

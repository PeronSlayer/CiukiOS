@echo off
setlocal
set "ROOT=%~dp0"
set "QEMU=%ROOT%qemu\qemu-system-i386w.exe"
set "DISK=%ROOT%CiukiOS.img"
if not exist "%QEMU%" goto missing
if not exist "%DISK%" goto missing
pushd "%ROOT%qemu"
"%QEMU%" -L "%ROOT%qemu\share" -name CiukiOS -machine pc,vmport=off,i8042=on,pcspk-audiodev=snd0 -cpu pentium3 -m 256 -drive "file=%DISK%,format=raw,if=ide" -boot c -vga none -device "VGA,xres=1280,yres=800" -display "gtk,gl=off,zoom-to-fit=on,show-menubar=off,window-close=on" -audiodev "dsound,id=snd0" -device "sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd0" -device "adlib,audiodev=snd0" -netdev "user,id=ciuknet0" -device "ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56" -no-reboot
set "RC=%ERRORLEVEL%"
popd
if "%RC%"=="0" exit /b 0
echo CiukiOS or QEMU exited with error %RC%.
pause
exit /b %RC%
:missing
echo CiukiOS.img or qemu\qemu-system-i386w.exe is missing.
echo Extract the entire ZIP before starting CiukiOS.
pause
exit /b 1

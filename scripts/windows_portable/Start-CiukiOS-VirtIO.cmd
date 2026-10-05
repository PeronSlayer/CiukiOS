@echo off
setlocal
set "ROOT=%~dp0"
set "QEMU=%ROOT%qemu\qemu-system-i386.exe"
if not exist "%QEMU%" set "QEMU=%ROOT%qemu\qemu-system-i386w.exe"
set "DISK=%ROOT%CiukiOS.img"

if not exist "%DISK%" (
    echo.
    echo ERROR: CiukiOS.img not found in "%ROOT%"!
    echo Make sure the entire ZIP is extracted into a normal folder before launching.
    echo.
    pause
    exit /b 1
)
if not exist "%QEMU%" (
    echo.
    echo ERROR: QEMU executable not found in "%ROOT%qemu"!
    echo Make sure the entire ZIP is extracted completely.
    echo.
    pause
    exit /b 1
)

pushd "%ROOT%qemu"
"%QEMU%" -L "%ROOT%qemu\share" -name "CiukiOS (VirtIO-GPU)" -machine pc,vmport=off,i8042=on,pcspk-audiodev=snd0 -cpu pentium3 -m 256 -device "virtio-rng-pci,disable-modern=on,disable-legacy=off" -drive "file=%DISK%,format=raw,if=ide" -boot c -vga none -device "virtio-vga,xres=1280,yres=800" -display "sdl,gl=off,window-close=on" -audiodev "sdl,id=snd0" -device "AC97,audiodev=snd0" -device "sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd0" -device "adlib,audiodev=snd0" -netdev "user,id=ciuknet0" -device "ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56" -no-reboot
set "RC=%ERRORLEVEL%"

if "%RC%"=="0" exit /b 0

echo.
echo [Audio] Host audio initialization failed (code %RC%).
echo Retrying in safe mode without host audio...
"%QEMU%" -L "%ROOT%qemu\share" -name "CiukiOS (VirtIO-GPU - No Sound)" -machine pc,vmport=off,i8042=on,pcspk-audiodev=snd0 -cpu pentium3 -m 256 -device "virtio-rng-pci,disable-modern=on,disable-legacy=off" -drive "file=%DISK%,format=raw,if=ide" -boot c -vga none -device "virtio-vga,xres=1280,yres=800" -display "sdl,gl=off,window-close=on" -audiodev "none,id=snd0" -device "AC97,audiodev=snd0" -device "sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd0" -device "adlib,audiodev=snd0" -netdev "user,id=ciuknet0" -device "ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56" -no-reboot
set "RC=%ERRORLEVEL%"
popd

if "%RC%"=="0" exit /b 0
echo.
echo CiukiOS or QEMU exited with error %RC%.
pause
exit /b %RC%

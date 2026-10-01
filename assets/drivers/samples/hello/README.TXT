HELLO - a sample CiukiOS driver package

Install it from the Control Panel: Drivers, Add New Driver, folder
C:\DRIVERS\SAMPLES\HELLO. LOADDRV.COM loads it the next time CiukiOS
starts. It stays resident and answers INT 2Fh AX=C000h with AX=C0DEh.

A driver package is a folder with DRIVER.INF:
  Name         up to eight letters or digits
  Class        up to eight letters or digits (AUDIO, VIDEO, NET, INPUT...)
  Version      shown in Devices
  Description  shown in Devices
  Files        the files to install, separated by commas
  Load         the command that loads it (a program among Files, and its
               options)
  Hardware     optional PCI ids (VVVV:DDDD, separated by commas)

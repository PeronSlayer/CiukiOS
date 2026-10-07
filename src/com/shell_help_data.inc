; Shared compressed command guide. HELP.RLE and COMMAND.COM use identical bytes.
msg_help    db '+', HELP_RLE, 24, 0x2D, ' CiukiOS command guide ', HELP_RLE, 25, 0x2D, '+', 0x0D, 0x0A
            db '| SYSTEM', HELP_RLE, 5, 0x20, 'HELP  VER  ECHO  CLS/CLEAR  REBOOT  SHUTDOWN', HELP_RLE, 16, 0x20, '|', 0x0D, 0x0A
            db '| NAVIGATION CD/CHDIR  DIR  PWD  PATH  WHERE <name>', HELP_RLE, 22, 0x20, '|', 0x0D, 0x0A
            db '| FILES', HELP_RLE, 6, 0x20, 'TYPE  COPY  DEL/ERASE  REN/RENAME/MOVE', HELP_RLE, 22, 0x20, '|', 0x0D, 0x0A
            db '| EDITOR', HELP_RLE, 5, 0x20, 'EDIT [file]', HELP_RLE, 7, 0x20, 'full-screen editor; F1 shows shortcuts', HELP_RLE, 4, 0x20, '|', 0x0D, 0x0A
            db '| DIRECTORIES MKDIR/MD  RMDIR/RD', HELP_RLE, 41, 0x20, '|', 0x0D, 0x0A
            db '| PROGRAMS   <name> [args] or RUN <name/path> [args]', HELP_RLE, 21, 0x20, '|', 0x0D, 0x0A
            db '+', HELP_RLE, 31, 0x2D, ' Network ', HELP_RLE, 32, 0x2D, '+', 0x0D, 0x0A
            db '| 1. NETSTART', HELP_RLE, 16, 0x20, 'start NIC + permanent ARP/ICMP service', HELP_RLE, 6, 0x20, '|', 0x0D, 0x0A
            db '| 2. IPCONFIG', HELP_RLE, 16, 0x20, 'show IPv4 values and resident ICMP status   |', 0x0D, 0x0A
            db '| 3. NETCFG STATIC <ip> <mask> <gateway> <dns>', HELP_RLE, 27, 0x20, '|', 0x0D, 0x0A
            db '|', HELP_RLE, 28, 0x20, 'save; live-apply when NETSTART is active', HELP_RLE, 4, 0x20, '|', 0x0D, 0x0A
            db '| 4. NETCFG DHCP', HELP_RLE, 13, 0x20, 'request lease and reload resident ICMP', HELP_RLE, 5, 0x20, '|', 0x0D, 0x0A
            db '| 5. PING <host>', HELP_RLE, 13, 0x20, 'send ICMP echo (gateway or Internet)', HELP_RLE, 8, 0x20, '|', 0x0D, 0x0A
            db '| 6. FTP <host> / FTPSRV', HELP_RLE, 5, 0x20, 'FTP client / start C:\SHARE server', HELP_RLE, 9, 0x20, '|', 0x0D, 0x0A
            db '|', HELP_RLE, 4, 0x20, 'PKTCHK / PKTTOOL', HELP_RLE, 8, 0x20, 'Packet Driver diagnostics', HELP_RLE, 19, 0x20, '|', 0x0D, 0x0A
            db '|', HELP_RLE, 4, 0x20, 'ICMP remains active after FTPSRV stops; TAP enables host ping', HELP_RLE, 7, 0x20, '|', 0x0D, 0x0A
            db '+', HELP_RLE, 72, 0x2D, '+', 0x0D, 0x0A
            db 'Power queue: SHUTDOWN/REBOOT <seconds|STATUS|CANCEL>. EXIT is disabled.', 0x0D, 0x0A, '$'

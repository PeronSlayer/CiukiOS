; CiukiOS Setup - Disk Detection and FAT Write
; TODO:
;   - detect_drives: enumerate INT 13h drives (FDD, HDD)
;   - detect_cd: probe INT 13h extended / ATAPI for CD-ROM
;   - write_mbr: write partition table to target drive
;   - format_fat16: write FAT16 BPB, FATs, root dir to target partition
;   - verify_disk_label: read volume label from inserted floppy, compare to expected

bits 16

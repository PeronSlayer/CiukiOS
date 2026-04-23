; CiukiOS Setup - Multi-media File Copy Engine
; TODO:
;   - parse_manifest: read SETUP.INF, build in-memory file list
;   - copy_loop: iterate file list, copy from source media to target FAT
;   - floppy_swap: prompt disk insert, verify label, resume copy
;   - cd_read_file: read file from CD-ROM via INT 13h extended
;   - checksum_verify: CRC16 or checksum check per file after copy
;   - write_file_fat: create FAT16 directory entry + write clusters on target

bits 16

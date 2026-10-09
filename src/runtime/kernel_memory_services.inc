; ABI2 service 12: immutable versioned memory ownership descriptor.
kernel_runtime_memory_layout_service:
    push cs
    pop ds
    mov si, kernel_memory_layout
    mov cx, kernel_memory_layout_end - kernel_memory_layout
    mov ax, 1
    clc
    retf

kernel_memory_layout:
    db 'CMLY'
    dw 1, kernel_memory_layout_end - kernel_memory_layout, 9, 8
    ; kind, segment, paragraphs, lifetime (1 resident, 2 boot, 4 dynamic)
    dw 1, CIUKIDOS_RUNTIME_SEG, CIUKIDOS_CORE_PARAS, 1
    dw 2, DOS_SYSVARS_SEG, 0x0070, 1
    dw 3, DOS_EXEC_STATE_BASE_SEG, 0x0100, 1
    dw 4, STAGE2_LOAD_SEG, 0x0020, 2
    dw 5, DOS_META_BUF_SEG, 0x0100, 1
    dw 6, DOS_FAT_BUF_SEG, 0x0100, 1
    dw 7, DOS_IO_BUF_SEG, 0x0030, 1
    dw 8, DOS_ENV_SEG, 0x0050, 1
    dw 9, COM_LOAD_SEG, 0, 4
kernel_memory_layout_end:

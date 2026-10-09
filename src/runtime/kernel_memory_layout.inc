; Full-profile paragraph reservations follow the actual resident core extent.
; None of the unused 60 KiB growth capacity is reserved from a DOS process.
%define CIUKIDOS_CORE_PARAS ((ciukidos_image_end - ciukidos_image_start + 15) / 16)
%define DOS_SYSVARS_SEG (CIUKIDOS_RUNTIME_SEG + CIUKIDOS_CORE_PARAS)
%define DOS_EXEC_STATE_BASE_SEG (DOS_SYSVARS_SEG + 0x0070)
%define DOS_EXEC_STATE_FRAME_PARAS 0x0016
%define DOS_EXEC_STATE_FRAME_MAX 11
%define STAGE2_LOAD_SEG (DOS_EXEC_STATE_BASE_SEG + 0x0080)
%define DOS_META_BUF_SEG (DOS_EXEC_STATE_BASE_SEG + 0x0100)
%define DOS_FAT_BUF_SEG (DOS_META_BUF_SEG + 0x0100)
%define DOS_IO_BUF_SEG (DOS_FAT_BUF_SEG + 0x0100)
%define DOS_ENV_SEG (DOS_IO_BUF_SEG + 0x0030)
%define COM_LOAD_SEG (DOS_ENV_SEG + 0x0050)
%define MZ_LOAD_SEG (COM_LOAD_SEG + 0x0080)
%define MZ2_LOAD_SEG 0x3200
%define MZ3_LOAD_SEG 0x7200
%define MZ_LOAD_LIMIT_SEG 0x5200
%define RUNTIME_LOAD_SEG CIUKIDOS_RUNTIME_SEG

; CiukiOS foreground text/cooperative graphics runtime, ABI 3.
; Offsets are module-relative.
; The module is a raw org-0 image loaded into a DOS-owned block, not a COM.
%ifndef CIUKIOS_DOS_WINDOW_ABI
%define CIUKIOS_DOS_WINDOW_ABI 1
DW_ABI_VERSION equ 3
DW_HEADER_BYTES equ 256
DW_MAGIC equ 4                       ; eight bytes "CWRT0001"
DW_VERSION equ 12                    ; word
DW_HEADER_SIZE equ 14                ; word
DW_IMAGE_BYTES equ 16                ; word, includes private storage/stack
DW_CELLS_OFFSET equ 18               ; word, 80*25 character/attribute pairs
DW_DIRTY_OFFSET equ 20               ; word, dword row mask; host consumes it
DW_CALLBACK_OFF equ 24               ; host FAR callback
DW_CALLBACK_SEG equ 26
DW_HOST_DS equ 28                    ; host DS; ES=runtime on callback entry
DW_INDOS_OFF equ 30                  ; optional diagnostic pointer; not a lock
DW_INDOS_SEG equ 32
DW_INSTALLED equ 34                  ; byte, runtime owned
DW_BUSY equ 35                       ; byte, host callback in progress
DW_FOCUSED equ 36                    ; byte, guest keyboard focus
DW_GUEST_LIVE equ 37                 ; byte, host sets around synchronous EXEC
DW_CURSOR equ 38                    ; word, BIOS DX: DH=row, DL=column
DW_CURSOR_SHAPE equ 40               ; word, BIOS CX
DW_MODE equ 42                       ; byte, logical BIOS text mode 0..3
DW_PAGE equ 43                       ; byte; only page zero is supported
DW_ERRORS equ 44                     ; word, sticky DW_ERR_* bits
DW_GRAPHICS_REQUEST equ 46           ; word, last unsupported mode request
DW_TICKS equ 48                      ; dword, IRQ0 observations
DW_CALLBACK_COUNT equ 52             ; dword, completed host callbacks
DW_FONT_OFFSET equ 56                ; word, 8192 bytes reserved for host font
DW_WIDTH equ 58                      ; word, host pixel dimensions
DW_HEIGHT equ 60                     ; word
DW_MOUSE_X equ 62                    ; word, current clamped host position
DW_MOUSE_Y equ 64                    ; word
DW_MOUSE_BUTTONS equ 66              ; word, physical mouse button bits
DW_HOST_UNSAFE equ 68                ; byte, host foreground mutation guard
DW_CLOSE_PENDING equ 69              ; byte, Esc requested until ring has space
DW_KEY_WAIT equ 70                   ; byte, guest blocked in INT16
DW_COLUMNS equ 71                   ; byte, logical 40 or 80 columns
DW_ROWS equ 72                      ; byte, 25
DW_INT10_DEPTH equ 73                ; byte, text update in progress
DW_LAST_HOST_RESULT equ 74           ; word, last callback AX
DW_CLOSE_REQUEST equ 76              ; byte, sticky cooperative close request
DW_PAINT_COUNT equ 80                ; dword, completed compositor paints
DW_CALLBACK_TSC equ 84               ; qword, elapsed TSC incl nested IRQs
DW_CALLBACK_MAX_TSC equ 92           ; qword
DW_PAINT_TSC equ 100                 ; qword
DW_PAINT_MAX_TSC equ 108             ; qword
DW_CALLBACK_START_TSC equ 116        ; private qword
DW_PAINT_START_TSC equ 124           ; private qword
DW_WAIT_SERVICE_COUNT equ 132        ; dword, blocking INT16 service entries
DW_MOUSE_CHANGE_COUNT equ 136        ; dword
DW_GFX_ACTIVE equ 192                ; byte, cooperative source-port client
DW_GFX_BPP equ 193                   ; byte, actual VBE bits per pixel
DW_GFX_BUFFER_SEG equ 194            ; word, parent-owned 64000-byte block
DW_GFX_MASKS equ 196                 ; six bytes, RGB size/position pairs
DW_GFX_DRAW equ 202                  ; FAR opaque indexed band painter
DW_GFX_DESCRIPTOR equ 206            ; word, module-relative CG ABI1 header
DW_GFX_PAINTED equ 208               ; byte, current callback completed paint
DW_CELL_ENTRY equ 160                ; FAR opaque-cell renderer, segment set by host
DW_CELL_BAND_SEG equ 164             ; current compositor band segment
DW_CELL_STRIDE equ 166               ; bytes per band scanline
DW_CELL_BAND_ROWS equ 168            ; row capacity, copied with the stride
DW_CELL_TOP equ 170                  ; current band bounds, exclusive bottom/right
DW_CELL_BOTTOM equ 172
DW_CELL_LEFT equ 174
DW_CELL_RIGHT equ 176
DW_CELL_PALETTE equ 178              ; host DS offset of 16 packed dword colors
DW_CELL_BYTES equ 180                ; native pixel size 1/2/3/4

DW_QUERY_AX equ 0xFF43               ; private INT10 query, exact AX/BX pair
DW_QUERY_BX equ 0x5744
DW_QUERY_REPLY equ 0x4357            ; AX reply; DX bit0=sticky close request

DW_OP_INSTALL equ 0
DW_OP_UNINSTALL equ 1
DW_OP_CLEAR equ 2
DW_OP_POP_KEY equ 3                  ; AX=BIOS key, CF=1 when empty
DW_OP_PEEK_KEY equ 4                 ; same without advancing the BIOS ring

DW_ERR_GRAPHICS equ 0x0001
DW_ERR_PAGE equ 0x0002
DW_ERR_VIDEO_API equ 0x0004
DW_ERR_REENTRANT equ 0x0008
DW_ERR_PROTECTED equ 0x0010
DW_ERR_KEYBOARD equ 0x0020
DW_ERR_MOUSE_API equ 0x0040

DW_HOST_TEXT_DIRTY equ 0x0001         ; callback input AX
DW_HOST_FOCUS equ 0x0001              ; callback result AX
DW_HOST_CLOSE equ 0x0002              ; request Esc; never force-terminate DOS
DW_HOST_REPAINT equ 0x0004            ; host still has pending damage

; Callback: DS=host shell, ES=runtime module, AX bit0=text dirty,
; BX=mouse buttons, CX=x, DX=y. Return FAR with AX bits above.
; No DOS, BIOS, EXEC or allocator calls from the callback. Memory/LFB only.
; Runtime saves all 32-bit registers/segment registers and invokes the host
; on a private 8KiB stack after chaining original IRQ0 and its EOI.
%endif

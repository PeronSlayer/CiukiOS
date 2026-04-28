; ciukedit.asm  -  CiukiDOS EDIT MVP
; Minimal line-oriented editor COM payload for CiukiOS
; Behavior:
;   - Print banner "[CIUKEDIT:BOOT]"
;   - Parse filename from PSP command tail (offset 0x80/0x81)
;   - If no filename: print usage, exit code 1
;   - Attempt open existing file (INT 21h AH=3Dh, read/write mode 2)
;   - If not found: create new file (INT 21h AH=3Ch)
;   - Read first 128 bytes of existing file and display
;   - Prompt user for one line of input (INT 21h AH=0Ah buffered)
;   - Seek to end and append entered line + CRLF
;   - Close file
;   - Print deterministic serial markers for test infrastructure
; Size constraint: fits in 2 x 512-byte clusters (floppy) / 1 x 8-sector cluster (full)
; NASM flat binary, org 0x0100 (DOS COM convention)

bits 16
org 0x0100

; ---------------------------------------------------------------------------
; Entry point
; ---------------------------------------------------------------------------
start:
    ; Print boot marker
    mov  dx, msg_boot
    mov  ah, 0x09
    int  0x21

    ; DS:0x80 = PSP command tail length byte
    ; DS:0x81 = command tail bytes (may start with spaces)
    xor  cx, cx
    mov  cl, [0x80]         ; tail length
    or   cx, cx
    jz   .no_arg_near       ; zero length => no arg

    mov  si, 0x81           ; pointer to tail

.skip_spaces:
    mov  al, [si]
    cmp  al, ' '
    jne  .found_arg
    inc  si
    loop .skip_spaces
    ; fell through loop with only spaces
.no_arg_near:
    jmp  .no_filename

.found_arg:
    ; Copy filename from tail into fname buffer (null-terminated)
    mov  di, fname
.copy_fname:
    mov  al, [si]
    cmp  al, ' '
    je   .fname_done
    cmp  al, 0x0D           ; CR
    je   .fname_done
    cmp  al, 0x00
    je   .fname_done
    mov  [di], al
    inc  si
    inc  di
    jmp  .copy_fname
.fname_done:
    mov  byte [di], 0       ; null-terminate

    ; Attempt open for read/write (mode 2)
    mov  dx, fname
    mov  ax, 0x3D02
    int  0x21
    jnc  .file_opened

    ; Open failed -> create new file (attrs=0)
    mov  dx, fname
    xor  cx, cx
    mov  ah, 0x3C
    int  0x21
    jc   .file_create_err

    mov  [fh], ax
    mov  dx, msg_new
    mov  ah, 0x09
    int  0x21
    jmp  .prompt_input

.file_opened:
    mov  [fh], ax
    mov  dx, msg_opened
    mov  ah, 0x09
    int  0x21

    ; Try to read first 128 bytes (best effort, ignore failure)
    mov  bx, [fh]
    mov  dx, read_buf
    mov  cx, 128
    mov  ah, 0x3F
    int  0x21
    jc   .prompt_input      ; read failure is non-fatal

    ; Ensure read bytes are printable: print if any bytes returned (AX = bytes read)
    or   ax, ax
    jz   .prompt_input
    push ax                  ; save actual read count
    mov  dx, msg_content_hdr
    mov  ah, 0x09
    int  0x21
    ; Null-terminate at read count for safe printing via custom routine
    pop  cx
    mov  si, read_buf
    call print_buf_cx       ; print CX bytes raw via TTY (AH=0Eh loop)
    mov  dx, msg_crlf
    mov  ah, 0x09
    int  0x21

.prompt_input:
    ; Prompt for one line
    mov  dx, msg_prompt
    mov  ah, 0x09
    int  0x21

    ; Buffered keyboard input: input_buf[0]=max, input_buf[1]=actual count
    mov  byte [input_buf], 126  ; max chars (not counting CR terminator)
    mov  byte [input_buf+1], 0
    mov  dx, input_buf
    mov  ah, 0x0A
    int  0x21

    ; Echo newline after input
    mov  dx, msg_crlf
    mov  ah, 0x09
    int  0x21

    ; Seek to end of file (for append)
    mov  bx, [fh]
    xor  cx, cx
    xor  dx, dx
    mov  ax, 0x4202         ; LSEEK from end
    int  0x21
    ; Non-fatal if seek fails (new file is at 0)

    ; Build write buffer: input line + CRLF
    xor  cx, cx
    mov  cl, [input_buf+1]  ; actual characters typed
    mov  si, input_buf+2    ; text starts here (after max+actual bytes)
    mov  di, write_buf
    jcxz .write_crlf_only   ; empty line: just write CRLF
    push cx
    rep  movsb
    pop  cx
.write_crlf_only:
    mov  byte [di],   0x0D  ; CR
    mov  byte [di+1], 0x0A  ; LF
    add  cx, 2

    ; Write to file
    mov  bx, [fh]
    mov  dx, write_buf
    mov  ah, 0x40
    int  0x21
    jc   .write_err

    ; Close file
    mov  bx, [fh]
    mov  ah, 0x3E
    int  0x21

    ; Success markers
    mov  dx, msg_ok
    mov  ah, 0x09
    int  0x21
    mov  ax, 0x4C00
    int  0x21

; ---------------------------------------------------------------------------
; Error paths
; ---------------------------------------------------------------------------
.no_filename:
    mov  dx, msg_usage
    mov  ah, 0x09
    int  0x21
    mov  ax, 0x4C01
    int  0x21

.file_create_err:
    mov  dx, msg_err_open
    mov  ah, 0x09
    int  0x21
    mov  ax, 0x4C02
    int  0x21

.write_err:
    mov  bx, [fh]
    mov  ah, 0x3E
    int  0x21
    mov  dx, msg_err_write
    mov  ah, 0x09
    int  0x21
    mov  ax, 0x4C03
    int  0x21

; ---------------------------------------------------------------------------
; Helper: print CX bytes from SI via BIOS TTY (AH=0Eh, not INT 21h strings)
; Preserves no registers beyond those pushed/popped internally.
; ---------------------------------------------------------------------------
print_buf_cx:
    jcxz .done
    push bx
    xor  bh, bh
.loop:
    lodsb
    mov  ah, 0x0E
    int  0x10
    loop .loop
    pop  bx
.done:
    ret

; ---------------------------------------------------------------------------
; Strings (INT 21h AH=09h format, '$'-terminated)
; ---------------------------------------------------------------------------
msg_boot        db "[CIUKEDIT:BOOT]", 0x0D, 0x0A,
                db "CiukiDOS EDIT MVP", 0x0D, 0x0A, '$'
msg_usage       db "Usage: CIUKEDIT <filename>", 0x0D, 0x0A,
                db "[CIUKEDIT:ERR-NOARG]", 0x0D, 0x0A, '$'
msg_new         db "[CIUKEDIT:NEW]", 0x0D, 0x0A, '$'
msg_opened      db "[CIUKEDIT:OPEN]", 0x0D, 0x0A, '$'
msg_content_hdr db "--- existing content ---", 0x0D, 0x0A, '$'
msg_prompt      db "Enter line> $"
msg_ok          db "[CIUKEDIT:OK]", 0x0D, 0x0A, '$'
msg_err_open    db "[CIUKEDIT:ERR-OPEN]", 0x0D, 0x0A, '$'
msg_err_write   db "[CIUKEDIT:ERR-WRITE]", 0x0D, 0x0A, '$'
msg_crlf        db 0x0D, 0x0A, '$'

; ---------------------------------------------------------------------------
; Data / BSS (allocated inline so binary includes space)
; ---------------------------------------------------------------------------
fh          dw 0                   ; file handle
fname       times 64  db 0         ; null-terminated filename from CLI
read_buf    times 130 db 0         ; first-read buffer (128 bytes + slack)
input_buf   times 130 db 0         ; AH=0Ah buffered input block
write_buf   times 132 db 0         ; output line buffer (128 + CRLF + slack)

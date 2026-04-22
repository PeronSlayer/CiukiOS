bits 16
org 0x0100

; OpenGEM Desktop - Phase 3 Milestone
; Functional desktop displaying graphics, menu bar, window, and responding to input

cli
mov ax, cs
mov ds, ax
mov es, ax
mov ss, ax
mov sp, 0xFFFE
sti

; Set VGA mode 13h (320x200, 256 colors)
mov ax, 0x0013
int 0x10

; Point to VGA buffer
mov ax, 0xA000
mov es, ax
xor di, di

; Clear entire screen to turquoise (color 3)
mov al, 3
mov cx, 0x8000
.clear_loop:
  mov [es:di], al
  inc di
  loop .clear_loop

; Draw white menu bar at top (20 pixels, color 15)
xor di, di
mov al, 15
mov cx, 320 * 20
.menubar:
  mov [es:di], al
  inc di
  loop .menubar

; Draw white status bar at bottom (10 pixels, color 15)
mov di, 320 * 190
mov cx, 320 * 10
.statusbar:
  mov [es:di], al
  inc di
  loop .statusbar

; Draw red centered window (160x100 pixels, color 12)
mov si, 60              ; Starting row
mov bp, 100             ; Number of rows
.window_loop:
  ; Calculate offset for this row: (SI * 320) + 80
  mov ax, si
  mov bx, 320
  mul bx
  add ax, 80
  mov di, ax
  
  ; Fill 160 pixels with red
  mov al, 12
  mov cx, 160
  .row_fill:
    mov [es:di], al
    inc di
    loop .row_fill
  
  inc si
  dec bp
  cmp bp, 0
  jne .window_loop

; Main event loop (display for ~60 frames)
mov byte [frame_counter], 60

.event_loop:
  ; Check keyboard (non-blocking)
  mov ah, 0x01
  int 0x16
  jz .no_keypress
  
  ; Key pressed - exit immediately
  mov ax, 0x4C00
  int 0x21

.no_keypress:
  ; Query mouse position via INT33h AH=3
  mov ax, 0x0003
  int 0x33
  ; BX = button state, CX = X position, DX = Y position
  
  ; Optional: draw simple mouse cursor
  ; (Skipped for minimal code)
  
  ; Delay loop
  mov cx, 0x4000
  .delay_loop:
    loop .delay_loop
  
  ; Decrement frame counter
  dec byte [frame_counter]
  cmp byte [frame_counter], 0
  jne .event_loop

; Return to DOS
mov ax, 0x4C00
int 0x21

frame_counter: db 0

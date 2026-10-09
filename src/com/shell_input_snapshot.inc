; Small loader for a separate diagnostic module, keeping SHELL's DOS arena
; unchanged. AL is the startup stage. No resident allocation or IRQ hook.
input_snapshot_save:
    pushad
    push ds
    push es
    push ax
    call setup_exec_block
    pop ax
    add al,'0'
    mov byte [exec_tail],2
    mov byte [exec_tail+1],' '
    mov [exec_tail+2],al
    mov byte [exec_tail+3],13
    mov dx,input_snapshot_module
    mov bx,exec_env_seg
    mov ax,0x4B00
    int 0x21
    pop es
    pop ds
    popad
    ret
input_snapshot_module db '\SYSTEM\INPUTST.COM',0

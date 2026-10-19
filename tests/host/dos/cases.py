"""Executable, real-mode DOS conformance scenarios; no DOS API emulation.

SPDX-License-Identifier: GPL-2.0-only
Each case is a separate COM process. Success requires semantic assertions,
not merely identical errors from two unimplemented functions. See README.md.
"""
from dataclasses import dataclass


@dataclass(frozen=True)
class Case:
    id: str
    functions: tuple[str, ...]
    purpose: str
    body: str
    fixture: str = "basic"


CASES = []


def case(id, functions, purpose, body, fixture="basic"):
    CASES.append(Case(id, tuple(functions.split()), purpose, body, fixture))


CREATE = """
    mov dx, file
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    OK
    mov [handle], ax
    mov bx, ax
"""
CLOSE = """
    mov bx, [handle]
    mov ah, 0x3e
    int 0x21
    OK
"""
WRITE = """
    mov dx, contents
    mov cx, 4
    mov ah, 0x40
    int 0x21
    OK
    EQ ax, 4
"""
REWIND = """
    mov bx, [handle]
    xor cx, cx
    xor dx, dx
    mov ax, 0x4200
    int 0x21
    OK
    EQ ax, 0
    EQ dx, 0
"""

case("handles", "21:3C 21:3D 21:3E 21:3F 21:40 21:41", "Create, write, reopen, exact contents, EOF, delete", CREATE + WRITE + CLOSE + """
    mov dx, file
    mov ax, 0x3d00
    int 0x21
    OK
    mov [handle], ax
    mov bx, ax
    mov dx, buffer
    mov cx, 8
    mov ah, 0x3f
    int 0x21
    OK
    EQ ax, 4
    EQ word [buffer], 0x4241
    EQ word [buffer+2], 0x4443
    mov cx, 1
    mov ah, 0x3f
    int 0x21
    OK
    EQ ax, 0
""" + CLOSE + """
    mov dx, file
    mov ah, 0x41
    int 0x21
    OK
    mov ax, 0x3d00
    int 0x21
    ERROR 2
""")
case("seek_gap", "21:42 21:3F 21:40", "Seek beyond EOF, read EOF, write extends; zero-write truncates", CREATE + WRITE + """
    xor cx, cx
    mov dx, 4096
    mov ax, 0x4200
    int 0x21
    OK
    EQ ax, 4096
    EQ dx, 0
    mov dx, buffer
    mov cx, 1
    mov ah, 0x3f
    int 0x21
    OK
    EQ ax, 0
""" + WRITE + """
    xor cx, cx
    xor dx, dx
    mov ax, 0x4202
    int 0x21
    OK
    EQ ax, 4100
""" + REWIND + """
    xor cx, cx
    mov ah, 0x40
    int 0x21
    OK
    EQ ax, 0
    xor dx, dx
    mov ax, 0x4202
    int 0x21
    OK
    EQ ax, 0
""" + CLOSE)
case("seek_signed", "21:42", "Signed CUR/END displacements", CREATE + WRITE + """
    mov cx, 0xffff
    mov dx, 0xfffe
    mov ax, 0x4201
    int 0x21
    OK
    EQ ax, 2
    EQ dx, 0
    mov cx, 0xffff
    mov dx, 0xffff
    mov ax, 0x4202
    int 0x21
    OK
    EQ ax, 3
""" + CLOSE)
case("duplicate", "21:45 21:46", "Dup shares position; force dup replaces a live handle", CREATE + WRITE + REWIND + """
    mov ah, 0x45
    int 0x21
    OK
    mov [other], ax
    mov bx, ax
    mov dx, buffer
    mov cx, 1
    mov ah, 0x3f
    int 0x21
    OK
    EQ ax, 1
    EQ byte [buffer], 'A'
    mov bx, [handle]
    xor cx, cx
    xor dx, dx
    mov ax, 0x4201
    int 0x21
    OK
    EQ ax, 1
    mov dx, second
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    OK
    mov cx, ax
    mov [third], ax
    mov bx, [handle]
    mov ah, 0x46
    int 0x21
    OK
    mov bx, [third]
    mov cx, 1
    mov dx, buffer
    mov ah, 0x3f
    int 0x21
    OK
    EQ byte [buffer], 'B'
    mov ah, 0x3e
    int 0x21
    OK
    mov bx, [other]
    mov ah, 0x3e
    int 0x21
    OK
""" + CLOSE)
case("exhaust", "21:3D 21:67", "JFT capacity failure is 4 and closes recover capacity", """
    mov bx, 20
    mov ah, 0x67
    int 0x21
    OK
    mov di, handles
    xor si, si
.open:
    mov dx, nul
    mov ax, 0x3d00
    int 0x21
    jc .full
    stosw
    inc si
    cmp si, 64
    jb .open
    jmp bad
.full:
    EQ ax, 4
    cmp si, 0
    je bad
.close:
    sub di, 2
    mov bx, [di]
    mov ah, 0x3e
    int 0x21
    OK
    dec si
    jnz .close
""")
case("invalid", "21:3E 21:3F 21:40 21:45 21:46 21:57 21:68", "Invalid handles fail without mutation", """
    mov bx, 0x7fff
    mov ah, 0x3e
    int 0x21
    ERROR 6
    mov dx, buffer
    mov cx, 1
    mov ah, 0x3f
    int 0x21
    ERROR 6
    mov ah, 0x40
    int 0x21
    ERROR 6
    mov ah, 0x45
    int 0x21
    ERROR 6
    mov cx, 7
    mov ah, 0x46
    int 0x21
    ERROR 6
    mov ax, 0x5700
    int 0x21
    ERROR 6
    mov ah, 0x68
    int 0x21
    ERROR 6
""")
case("directories", "21:39 21:3A 21:3B 21:47 21:56 21:43", "Directory attrs, cwd, nonempty rmdir, cross-directory rename", """
    mov dx, dir1
    mov ah, 0x39
    int 0x21
    OK
    mov dx, dir2
    mov ah, 0x39
    int 0x21
    OK
    mov dx, dir1
    mov ax, 0x4300
    int 0x21
    OK
    test cx, 0x10
    jz bad
    mov ah, 0x3b
    int 0x21
    OK
    xor dx, dx
    mov si, buffer
    mov ah, 0x47
    int 0x21
    OK
    mov si, buffer
    mov cx, 64
.cwd_scan:
    cmp byte [si], 0
    je .cwd_end
    inc si
    loop .cwd_scan
    jmp bad
.cwd_end:
    cmp si, buffer+7
    jb bad
    EQ word [si-7], 0x3444
    EQ word [si-5], 0x4438
    EQ word [si-3], 0x5249
    EQ byte [si-1], '1'
    mov dx, parent
    mov ah, 0x3b
    int 0x21
    OK
""" + CREATE + CLOSE + """
    mov dx, file
    mov di, moved
    mov ah, 0x56
    int 0x21
    OK
    mov dx, dir2
    mov ah, 0x3a
    int 0x21
    ERROR 5
    mov dx, moved
    mov ah, 0x41
    int 0x21
    OK
    mov dx, dir1
    mov ah, 0x3a
    int 0x21
    OK
    mov dx, dir2
    mov ah, 0x3a
    int 0x21
    OK
""")
case("attributes", "21:43 21:3D 21:57 21:68", "Read-only rejection; exact packed timestamp survives close", CREATE + WRITE + """
    mov cx, 0x645c
    mov dx, 0x5821
    mov ax, 0x5701
    int 0x21
    OK
    mov ah, 0x68
    int 0x21
    OK
""" + CLOSE + """
    mov dx, file
    mov cx, 1
    mov ax, 0x4301
    int 0x21
    OK
    mov ax, 0x3d01
    int 0x21
    ERROR 5
    mov ax, 0x3d00
    int 0x21
    OK
    mov bx, ax
    mov [handle], ax
    mov ax, 0x5700
    int 0x21
    OK
    EQ cx, 0x645c
    EQ dx, 0x5821
""" + CLOSE + """
    mov dx, file
    xor cx, cx
    mov ax, 0x4301
    int 0x21
    OK
""")
case("find_dta", "21:1A 21:2F 21:4E 21:4F", "43-byte DTA guards, copied cursor, two interleaved searches", CREATE + WRITE + CLOSE + """
    mov dx, dta1
    mov ah, 0x1a
    int 0x21
    mov ah, 0x2f
    int 0x21
    EQ bx, dta1
    mov ax, es
    mov bx, ds
    EQ ax, bx
    mov dx, file
    xor cx, cx
    mov ah, 0x4e
    int 0x21
    OK
    EQ word [dta1+0x1a], 4
    EQ word [dta1+0x1c], 0
    EQ byte [dta1+43], 0xa5
    mov si, dta1
    mov di, dta2
    mov cx, 43
    rep movsb
    mov dx, dta3
    mov ah, 0x1a
    int 0x21
    mov dx, missing
    xor cx, cx
    mov ah, 0x4e
    int 0x21
    ERROR 18
    mov dx, dta2
    mov ah, 0x1a
    int 0x21
    mov ah, 0x4f
    int 0x21
    ERROR 18
    EQ byte [dta2+43], 0xa5
    mov dx, 0x80
    mov ah, 0x1a
    int 0x21
""")
case("find_paths", "21:4E 21:4F", "Empty directory versus absent parent", """
    mov dx, dir1
    mov ah, 0x39
    int 0x21
    OK
    mov dx, dta1
    mov ah, 0x1a
    int 0x21
    mov dx, empty_pattern
    xor cx, cx
    mov ah, 0x4e
    int 0x21
    ERROR 18
    mov dx, absent_pattern
    mov ah, 0x4e
    int 0x21
    ERROR 3
    mov dx, 0x80
    mov ah, 0x1a
    int 0x21
    mov dx, dir1
    mov ah, 0x3a
    int 0x21
    OK
""")
case("new_temp", "21:5A 21:5B", "Create-new collision and writable temporary-name buffer", """
    mov dx, file
    xor cx, cx
    mov ah, 0x5b
    int 0x21
    OK
    mov [handle], ax
""" + CLOSE + """
    mov dx, file
    xor cx, cx
    mov ah, 0x5b
    int 0x21
    ERROR 80
    mov dx, temp_path
    xor cx, cx
    mov ah, 0x5a
    int 0x21
    OK
    mov [handle], ax
""" + CLOSE + """
    mov dx, temp_path
    mov ah, 0x41
    int 0x21
    OK
""")
for action, suffix in ((0x10, "create"), (0x11, "open"), (0x12, "replace")):
    pre = "" if action == 0x10 else CREATE + WRITE + CLOSE
    expected = 2 if action == 0x10 else 1 if action == 0x11 else 3
    case("extended_" + suffix, "21:6C", "6C00 action/status " + suffix, pre + f"""
    mov si, file
    mov bx, 0x42
    xor cx, cx
    mov dx, {action}
    mov ax, 0x6c00
    int 0x21
    OK
    EQ cx, {expected}
    mov [handle], ax
    mov bx, ax
    xor cx, cx
    xor dx, dx
    mov ax, 0x4202
    int 0x21
    OK
    EQ ax, {4 if action == 0x11 else 0}
""" + CLOSE)
case("locks", "21:5C", "Lock/unlock exact range above 64 KiB, invalid unlock", CREATE + """
    mov cx, 1
    mov dx, 7
    xor si, si
    mov di, 16
    mov ax, 0x5c00
    int 0x21
    OK
    mov ax, 0x5c01
    int 0x21
    OK
    mov ax, 0x5c01
    int 0x21
    ERROR 33
""" + CLOSE, "share")
for deny, mode in (("all", 0x12), ("write", 0x22), ("read", 0x32), ("none", 0x42)):
    second_access = 0 if deny == "read" else 1
    case("deny_" + deny, "21:3D", "SHARE bidirectional deny-" + deny, CREATE + CLOSE + f"""
    mov dx, file
    mov ax, {0x3d00 | mode}
    int 0x21
    OK
    mov [handle], ax
    mov dx, file
    mov ax, {0x3d40 | second_access}
    int 0x21
""" + ("    OK\n    mov bx, ax\n    mov ah, 0x3e\n    int 0x21\n    OK\n" if deny == "none" else "    ERROR 5\n") + CLOSE, "share")
case("extended_error", "21:59", "59h retains missing-file detail; no nondeterministic registers", """
    mov dx, missing
    mov ax, 0x3d00
    int 0x21
    ERROR 2
    xor bx, bx
    mov ah, 0x59
    int 0x21
    push cs
    pop ds
    push cs
    pop es
    EQ ax, 2
    EQ bh, 8
    EQ bl, 3
    EQ ch, 2
""")
case("drives", "21:0E 21:19 21:36", "Drive selection and free-space layout", """
    mov ah, 0x19
    int 0x21
    EQ al, 2
    mov dl, 2
    mov ah, 0x0e
    int 0x21
    cmp al, 3
    jb bad
    mov dl, 3
    mov ah, 0x36
    int 0x21
    cmp ax, 0xffff
    je bad
    EQ cx, 512
""")
case("ioctl", "21:44", "4400 device bit distinguishes disk handles and NUL", CREATE + """
    mov ax, 0x4400
    int 0x21
    OK
    test dx, 0x80
    jnz bad
""" + CLOSE + """
    mov dx, nul
    mov ax, 0x3d02
    int 0x21
    OK
    mov [handle], ax
    mov bx, ax
    mov ax, 0x4400
    int 0x21
    OK
    test dx, 0x80
    jz bad
""" + CLOSE)
case("psp_vectors", "21:25 21:35 21:50 21:51 21:52 21:5D 21:62", "PSP identity, stable vector, valid list/SDA pointers", """
    mov ah, 0x62
    int 0x21
    mov ax, cs
    EQ bx, ax
    mov [saved_psp], bx
    mov ah, 0x51
    int 0x21
    EQ bx, word [saved_psp]
    mov ah, 0x50
    int 0x21
    mov ax, 0x3560
    int 0x21
    mov [vector], bx
    mov [vector+2], es
    push ds
    mov dx, handler
    mov ax, 0x2560
    int 0x21
    mov ax, 0x3560
    int 0x21
    EQ bx, handler
    lds dx, [vector]
    mov ax, 0x2560
    int 0x21
    pop ds
    mov ah, 0x52
    int 0x21
    mov ax, es
    test ax, ax
    jz bad
    mov ax, 0x5d06
    int 0x21
    push cs
    pop ds
    push cs
    pop es
    cmp cx, 1
    jb bad
""")
case("memory", "21:48 21:49 21:4A", "Resize, allocate/free and restore PSP context", """
    mov bx, 32
    mov ah, 0x48
    int 0x21
    OK
    mov es, ax
    mov bx, 16
    mov ah, 0x4a
    int 0x21
    OK
    mov ah, 0x49
    int 0x21
    OK
    push cs
    pop es
""")
case("nls", "21:65 21:66", "CP437 country tables and uppercasing", """
    mov bx, 0xffff
    mov dx, 0xffff
    mov cx, 41
    mov di, buffer
    mov ax, 0x6501
    int 0x21
    OK
    EQ byte [buffer], 1
    mov dl, 'q'
    mov ax, 0x6520
    int 0x21
    OK
    EQ dl, 'Q'
    mov ax, 0x6601
    int 0x21
    OK
    EQ bx, 437
    EQ dx, 437
""")
case("truename", "21:60", "Canonical absolute name", """
    mov si, file
    mov di, buffer
    mov ah, 0x60
    int 0x21
    OK
    EQ word [buffer], 0x3a43
    EQ byte [buffer+2], 0x5c
""")
# FCB tests use default record size 128, then explicitly select size 4.
case("fcb_io", "21:0F 21:10 21:14 21:15 21:16 21:21 21:22 21:23 21:24 21:27 21:28 21:29", "Sequential/random/block FCB I/O and file size", """
    mov si, file
    mov di, fcb
    mov ax, 0x2900
    int 0x21
    EQ al, 0
    mov dx, fcb
    mov ah, 0x16
    int 0x21
    EQ al, 0
    mov word [fcb+14], 4
    mov dx, contents
    mov ah, 0x1a
    int 0x21
    mov dx, fcb
    mov ah, 0x15
    int 0x21
    EQ al, 0
    mov ah, 0x24
    int 0x21
    EQ word [fcb+33], 1
    mov word [fcb+33], 0
    mov ah, 0x22
    int 0x21
    EQ al, 0
    mov ah, 0x10
    int 0x21
    EQ al, 0
    mov ah, 0x0f
    int 0x21
    EQ al, 0
    mov word [fcb+14], 4
    mov word [fcb+12], 0
    mov byte [fcb+32], 0
    mov dx, buffer
    mov ah, 0x1a
    int 0x21
    mov dx, fcb
    mov ah, 0x14
    int 0x21
    EQ al, 0
    EQ word [buffer], 0x4241
    mov word [fcb+33], 0
    mov ah, 0x21
    int 0x21
    EQ al, 0
    mov cx, 1
    mov ah, 0x27
    int 0x21
    EQ al, 0
    EQ cx, 1
    mov word [fcb+33], 0
    mov cx, 1
    mov ah, 0x28
    int 0x21
    EQ al, 0
    EQ cx, 1
    mov ah, 0x23
    int 0x21
    EQ al, 0
    EQ word [fcb+33], 1
    mov ah, 0x10
    int 0x21
    EQ al, 0
    mov dx, 0x80
    mov ah, 0x1a
    int 0x21
""")
case("fcb_names", "21:11 21:12 21:13 21:17", "FCB search/rename/delete", CREATE + CLOSE + """
    mov si, file
    mov di, fcb
    mov ax, 0x2900
    int 0x21
    EQ al, 0
    mov dx, dta1
    mov ah, 0x1a
    int 0x21
    mov dx, fcb
    mov ah, 0x11
    int 0x21
    EQ al, 0
    mov ah, 0x12
    int 0x21
    EQ al, 0xff
    mov si, rename_name
    mov di, fcb+17
    mov cx, 11
    rep movsb
    mov ah, 0x17
    int 0x21
    EQ al, 0
    mov si, rename_name
    mov di, fcb+1
    mov cx, 11
    rep movsb
    mov ah, 0x13
    int 0x21
    EQ al, 0
    mov dx, 0x80
    mov ah, 0x1a
    int 0x21
""")
case("multiplex", "2F:1000 2F:1100 2F:1A00 2F:1680 2F:1687 2F:4300 2F:4310", "SHARE installed, redirector/ANSI absent, DPMI/XMS available", """
    mov ax, 0x1000
    int 0x2f
    EQ al, 0xff
    mov ax, 0x1100
    int 0x2f
    EQ al, 0
    mov ax, 0x1a00
    int 0x2f
    EQ al, 0
    mov ax, 0x1680
    int 0x2f
    mov ax, 0x1687
    int 0x2f
    EQ ax, 0
    mov ax, 0x4300
    int 0x2f
    EQ al, 0x80
    mov ax, 0x4310
    int 0x2f
    mov ax, es
    test ax, ax
    jz bad
    push cs
    pop es
""", "share")
# Inject the same one-shot/persistent read fault in each backend. These
# deliberately fail if the fixture/hook is missing; they never return SKIP.
for retry in (False, True):
    case("critical_" + ("retry" if retry else "fail"), "24:* 21:59", "INT24 " + ("Retry once succeeds" if retry else "Fail returns an error"), f"""
    mov ax, 0x3524
    int 0x21
    mov [vector], bx
    mov [vector+2], es
    push cs
    pop es
    mov dx, critical
    mov ax, 0x2524
    int 0x21
    mov byte [critical_reply], {1 if retry else 3}
    mov dx, fault_path
    mov ax, 0x3d00
    int 0x21
    OK
    mov [handle], ax
    mov bx, ax
    mov dx, buffer
    mov cx, 4096
    mov ah, 0x3f
    int 0x21
    {'OK' if retry else 'jc .failed' }
    {'' if retry else 'jmp bad'}
.failed:
    {"EQ ax, 4096" if retry else "cmp ax, 0"}
    {"EQ word [buffer], 0x4646" if retry else "je bad"}
    EQ byte [critical_count], 1
    push ds
    lds dx, [vector]
    mov ax, 0x2524
    int 0x21
    pop ds
""" + CLOSE, "fault-once" if retry else "fault-always")


case("inherit_locks", "21:3D 21:4B 21:4C 21:4D 21:5C", "EXEC inheritance/private bit, shared offset, cross-PSP lock denial", CREATE + WRITE + CLOSE + """
    mov dx, file
    mov ax, 0x3d42
    int 0x21
    OK
    mov [handle], ax
    EQ word [handle], 5
    mov dx, file
    mov ax, 0x3dc2
    int 0x21
    OK
    EQ ax, 6
    mov [other], ax
    mov bx, [handle]
    xor cx, cx
    mov dx, 1
    mov ax, 0x4200
    int 0x21
    OK
    mov cx, 1
    mov dx, 7
    xor si, si
    mov di, 16
    mov ax, 0x5c00
    int 0x21
    OK
    mov [exec_tail+2], cs
    mov [exec_fcb1+2], cs
    mov [exec_fcb2+2], cs
    mov dx, child
    mov bx, exec_block
    mov ax, 0x4b00
    int 0x21
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    OK
    mov ah, 0x4d
    int 0x21
    EQ ax, 0
    mov bx, [handle]
    xor cx, cx
    xor dx, dx
    mov ax, 0x4201
    int 0x21
    OK
    EQ ax, 2
    mov cx, 1
    mov dx, 7
    xor si, si
    mov di, 16
    mov ax, 0x5c01
    int 0x21
    OK
    mov bx, [other]
    mov ah, 0x3e
    int 0x21
    OK
""" + CLOSE, "share")
case("critical_abort", "24:* 21:4B 21:4D", "INT24 Abort terminates only the child", """
    mov byte [tail], 2
    mov word [tail+1], 0x4120
    mov byte [tail+3], 13
    mov [exec_tail+2], cs
    mov [exec_fcb1+2], cs
    mov [exec_fcb2+2], cs
    mov dx, child
    mov bx, exec_block
    mov ax, 0x4b00
    int 0x21
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    OK
    mov ah, 0x4d
    int 0x21
    EQ ah, 2
""", "fault-always")
case("environment", "21:0D 21:1B 21:1C 21:2A 21:2C 21:30 21:33 21:34 21:38 21:54 21:58", "Loader environment queries; pointers remain inside VM", """
    mov ah, 0x0d
    int 0x21
    mov ah, 0x1b
    int 0x21
    cmp al, 0xff
    je bad
    mov dl, 3
    mov ah, 0x1c
    int 0x21
    cmp al, 0xff
    je bad
    push cs
    pop ds
    mov ax, 0x3000
    int 0x21
    cmp al, 3
    jb bad
    mov ax, 0x3300
    int 0x21
    cmp dl, 1
    ja bad
    mov ah, 0x34
    int 0x21
    mov ax, es
    test ax, ax
    jz bad
    push cs
    pop es
    mov dx, buffer
    mov ax, 0x3800
    int 0x21
    OK
    mov ah, 0x54
    int 0x21
    cmp al, 1
    ja bad
    mov ax, 0x5800
    int 0x21
    OK
    cmp ax, 2
    ja bad
    mov ah, 0x2a
    int 0x21
    cmp cx, 1980
    jb bad
    mov ah, 0x2c
    int 0x21
    cmp ch, 23
    ja bad
""")

case("console", "21:02 21:09 21:0B", "Console output and idle keyboard query still chain", """
    mov dl, ' '
    mov ah, 0x02
    int 0x21
    mov dx, empty_string
    mov ah, 0x09
    int 0x21
    mov ah, 0x0b
    int 0x21
    EQ al, 0
""")
case("console_input", "21:01 21:08", "Controller injects X then Y through virtual keyboard", """
    mov si, keys_ready
    call puts
    mov ah, 0x01
    int 0x21
    EQ al, 'X'
    mov ah, 0x08
    int 0x21
    EQ al, 'Y'
""", "keys-XY")

case("sft_jft", "2F:1203 2F:1216 2F:1220 21:52", "Read actual guest JFT/SFT mirror after write", CREATE + WRITE + """
    mov bx, [handle]
    mov ax, 0x1220
    int 0x2f
    OK
    xor bx, bx
    mov bl, [es:di]
    cmp bl, 0xff
    je bad
    mov ax, 0x1216
    int 0x2f
    OK
    cmp word [es:di], 0
    je bad
    EQ word [es:di+0x11], 4
    EQ word [es:di+0x13], 0
    EQ word [es:di+0x15], 4
    EQ word [es:di+0x17], 0
    mov ax, 0x1203
    int 0x2f
    mov ax, ds
    test ax, ax
    push cs
    pop ds
    jz bad
    push cs
    pop es
""" + CLOSE)
case("create_psp", "21:26 21:55", "Separate PSP creation services preserve selected parent", """
    mov ah, 0x62
    int 0x21
    mov [saved_psp], bx
    mov bx, 32
    mov ah, 0x48
    int 0x21
    OK
    mov [other], ax
    mov dx, ax
    mov ah, 0x26
    int 0x21
    mov es, [other]
    EQ word [es:0], 0x20cd
    mov dx, [other]
    mov si, dx
    add si, 32
    mov ah, 0x55
    int 0x21
    mov bx, [saved_psp]
    mov ah, 0x50
    int 0x21
    mov es, [other]
    EQ word [es:0], 0x20cd
    mov bx, [saved_psp]
    EQ word [es:0x16], bx
    mov ah, 0x49
    int 0x21
    OK
    push cs
    pop es
""")

TEMPLATE = r"""; Generated from tests/host/dos/cases.py. GPL-2.0-only
bits 16
org 0x100
%macro OK 0
    jc failed
%endmacro
%macro ERROR 1
    jnc bad
    cmp ax, %1
    jne bad
%endmacro
%macro EQ 2
    cmp %1, %2
    jne bad
%endmacro
start:
    cld
    ; Reference COM1: 38400 baud, 8N1, bounded polling in putc.
    mov dx, 0x3fb
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 3
    out dx, al
    inc dx
    xor al, al
    out dx, al
    mov dx, 0x3fb
    mov al, 3
    out dx, al
    mov ax, cs
    mov ds, ax
    mov es, ax
    cli
    mov ss, ax
    mov sp, stack_top
    sti
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4a
    int 0x21
    OK
{body}
    mov si, passed
    jmp report
bad:
    mov ax, 0xffff
failed:
    push cs
    pop ds
    mov [result], ax
    mov si, failure
report:
    push cs
    pop ds
    call puts
    mov ax, [result]
    call hex
    mov si, newline
    call puts
    cmp word [result], 0
    mov ax, 0x4c00
    je .exit
    inc al
.exit:
    int 0x21
puts:
    lodsb
    test al, al
    jz .done
    call putc
    jmp puts
.done:
    ret
putc:
    push ax
    push cx
    push dx
    ; E9 is Ciuki's existing per-VM record transport. UART is the bare
    ; FreeDOS reference transport (discarded by Ciuki's absent UART model).
    out 0xe9, al
    mov ah, al
    mov cx, 0x1000
.wait:
    mov dx, 0x3fd
    in al, dx
    test al, 0x20
    jnz .ready
    loop .wait
    jmp .done
.ready:
    mov dx, 0x3f8
    mov al, ah
    out dx, al
.done:
    pop dx
    pop cx
    pop ax
    ret
hex:
    mov bx, ax
    mov cx, 4
.next:
    rol bx, 4
    mov al, bl
    and al, 15
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    push cx
    call putc
    pop cx
    loop .next
    ret
handler:
    iret
critical:
    inc byte [cs:critical_count]
    mov al, [cs:critical_reply]
    iret
passed db '{tag} PASS ',0
failure db '{tag} FAIL ',0
newline db 13,10,0
result dw 0
handle dw 0
other dw 0
third dw 0
saved_psp dw 0
vector dd 0
critical_reply db 3
critical_count db 0
empty_string db '$'
keys_ready db 'DOSKEY READY',13,10,0
child db 'C:\D48\D48CHILD.COM',0
exec_block dw 0
exec_tail dw tail,0
exec_fcb1 dw fcb,0
exec_fcb2 dw fcb,0
tail db 0,13,0,0
file db 'D48FILE.DAT',0
second db 'D48OTHER.DAT',0
missing db 'D48NONE.XYZ',0
dir1 db 'D48DIR1',0
dir2 db 'D48DIR2',0
parent db '..',0
moved db 'D48DIR2\MOVED.DAT',0
empty_pattern db 'D48DIR1\*.XYZ',0
absent_pattern db 'D48NONE\*.XYZ',0
temp_path db '.\',0
    times 32 db 0
fault_path db 'E:\D48FAULT.DAT',0
contents db 'ABCD'
nul db 'NUL',0
rename_name db 'D48REN  DAT'
fcb times 48 db 0
handles times 64 dw 0
dta1 times 43 db 0
    db 0xa5
dta2 times 43 db 0
    db 0xa5
dta3 times 43 db 0
    db 0xa5
buffer times 4096 db 0
    times 1024 db 0
stack_top:
image_end:
"""


def source(index, item):
    return TEMPLATE.format(body=item.body, tag=f"D48{index:03d}")

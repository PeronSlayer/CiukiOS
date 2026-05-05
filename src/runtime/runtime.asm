bits 16
org 0x0000

runtime_start:
    jmp short runtime_entry

runtime_signature db 'CIUKRT01'
runtime_version db 'CiukiOS runtime split placeholder v0.1', 0

runtime_entry:
    retf

# ABI Handoff - UEFI Loader -> stage2 (Draft v0)

## Purpose
Define a stable contract between the UEFI loader and the DOS-like stage2.

## Constraints
1. Handoff happens after `ExitBootServices`.
2. No UEFI runtime dependency inside stage2.
3. All pointers must be valid in identity-mapped memory.

## Calling Convention (Proposal)
Current loader architecture: x86_64.

Registers on stage2 entry:
1. `RDI` = pointer to `boot_info_t`.
2. `RSI` = pointer to `handoff_v0_t`.

Required state:
1. `CR3` already set to valid bootstrap page tables.
2. `IF=0` (interrupts disabled during stage2 bootstrap).
3. Valid stack set by caller or stage2 prologue.

## `handoff_v0_t` Structure (Proposal)
```c
typedef struct handoff_v0 {
    uint64_t magic;               // "CIUKHOF0"
    uint64_t version;             // 0
    uint64_t stage2_load_addr;    // stage2 base address in RAM
    uint64_t stage2_size;         // size in bytes
    uint64_t flags;               // reserved
} handoff_v0_t;
```

## ABI Compatibility Rules
1. Add new fields only at the tail.
2. `magic` and `version` are mandatory.
3. Never reinterpret existing fields.

## Minimal Debug Checkpoints
1. Loader prints stage2 address + size.
2. Stage2 prints received `magic`, `version`.
3. Stage2 prints received `boot_info.magic`.

## v0 Success Criteria
1. Stage2 starts.
2. Stage2 reads handoff struct.
3. Stage2 reaches halt loop without faults.

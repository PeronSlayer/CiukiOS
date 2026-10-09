# E500 firmware replay

Decoded captures, not emulator output. Duplicate binary data sets are coalesced;
all source capture names and raw SHA-256 values are retained in `replay.json`.

CMAP: 16-byte little-endian `<4sBBHHHHH>` header (magic, version, status,
count, conventional KiB, EBDA segment, entry size, reserved), followed by
24-byte `<QQII>` E820 records. Only complete status 0 was imported.

CVT1: repeated 272-byte records: 16-byte trace header followed by the
unaltered 256-byte VBE ModeInfo fields. The trace contains no controller block.
Controller signature/version/capacity/capabilities/mode pointer come from the
same capture's CVB1 `SYSTEM/VIDEO/DISPLAY.LOG`: 32-byte header, 256-byte
ModeInfo, 512-byte controller block, 128-byte EDID. OEM pointers are omitted.

ACTIVE.CFG supplies PCI vendor/device/subsystem IDs and PCI BIOS availability.
Classification uses PCI identity, never the misleading archive path `t23-next`.
No disk identities, serial numbers, OEM strings or personal data are included.

Expected outputs are regression baselines for the reference model, not proof
of a new loader or kernel. Legacy trace results are historical metadata and
do not override the new eligibility rules. The mode list is only what was
captured; it is not claimed to be a complete firmware enumeration.

Distinct E820 maps: 1; distinct video data sets: 3.

Raw-file provenance (capture / file / SHA-256):

- `physical-logs-20261009-diskseq37-r15/DRIVERS/ACTIVE.CFG`: `866e0f1d865345ee743a06c9d06435aee5c0e28a38bdbd772e992028f1cab53a`
- `physical-logs-20261009-diskseq38-r16/DRIVERS/ACTIVE.CFG`: `866e0f1d865345ee743a06c9d06435aee5c0e28a38bdbd772e992028f1cab53a`
- `physical-logs-20261009-diskseq39-r17/DRIVERS/ACTIVE.CFG`: `866e0f1d865345ee743a06c9d06435aee5c0e28a38bdbd772e992028f1cab53a`
- `physical-logs-20261009-diskseq37-r15/SYSTEM/MEMMAP.BIN`: `5135aaa2ff998cef568c77b842a84d121014fee2ffb2d0425d00e1f8cb67a2b6`
- `physical-logs-20261009-diskseq38-r16/SYSTEM/MEMMAP.BIN`: `5135aaa2ff998cef568c77b842a84d121014fee2ffb2d0425d00e1f8cb67a2b6`
- `physical-logs-20261009-diskseq39-r17/SYSTEM/MEMMAP.BIN`: `5135aaa2ff998cef568c77b842a84d121014fee2ffb2d0425d00e1f8cb67a2b6`
- `physical-logs-20261009-diskseq37-r15/SYSTEM/VIDEO/VBE.TRC`: `9c261163112ab3f1449de201af7c7f3c43fb5dd4c71bd3a29cf1a8810a47edf1`
- `physical-logs-20261009-diskseq37-r15/SYSTEM/VIDEO/DISPLAY.LOG`: `212d8c3a69560525612dd8cd44c394d6d6b1a101ffe154a14bca22b3902016e0`
- `physical-logs-20261009-diskseq38-r16/SYSTEM/VIDEO/VBE.TRC`: `9c261163112ab3f1449de201af7c7f3c43fb5dd4c71bd3a29cf1a8810a47edf1`
- `physical-logs-20261009-diskseq38-r16/SYSTEM/VIDEO/DISPLAY.LOG`: `f693e035a15788a007a3e994d4727d1c42fb4d20f51ce2df2ce0c2a0ebf45836`
- `physical-logs-20261009-diskseq39-r17/SYSTEM/VIDEO/VBE.TRC`: `9c261163112ab3f1449de201af7c7f3c43fb5dd4c71bd3a29cf1a8810a47edf1`
- `physical-logs-20261009-diskseq39-r17/SYSTEM/VIDEO/DISPLAY.LOG`: `8d8fac2ac2b81ac893c94d53866df04399fe7846e1a8f5dd4571f5bfb346891e`

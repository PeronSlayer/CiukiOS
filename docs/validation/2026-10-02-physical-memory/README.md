# Physical E820 capture for the native memory redesign

The prepared `HARD` image is
`build/hardware/native-memory-20261002/ciukios-memory-map-prefix-v2.img`
(SHA-256 `bc50cc7203571c547138648b64774a9e56f07fa822095a99c0d372e2ad185744`).
It boots to the desktop in a 512 MiB QEMU test and writes a complete E820 map
at `C:\SYSTEM\MEMMAP.BIN`. The serial-pinned installer backs up the first
134,249,984 bytes of the Transcend SSD, writes the image, flushes it and
checks a complete physical readback. It requires root privileges.

The owner ran the installer on 2 October 2026. Its receipt records a complete
134,249,984-byte physical readback matching the image SHA-256 above; the
previous prefix was backed up with SHA-256
`b125e0f78e63f0a5326d126c981c75c77cc5da1b3ebefc08624dddd9d9a8cbb8`.
The SSD was then powered off cleanly. The owner subsequently booted the Compaq
E500 first. Its map was saved as `compaq-e500-MEMMAP.BIN` and decoded in
`compaq-e500.json`: six complete, nonoverlapping entries and 254.9375 MiB
usable above 1 MiB. `compaq-e500-BOOT.LOG` preserves the shell progress
markers. The owner reports no screen flashing, but keyboard and mouse fail
in this boot. The IBM T23 was then booted and captured: `ibm-t23-MEMMAP.BIN`
and `ibm-t23.json` contain ten complete, nonoverlapping entries and
510.375 MiB usable above 1 MiB; `ibm-t23-BOOT.LOG` preserves its boot log.
The owner reports a visible desktop and working keyboard and mouse on the T23.
The SSD was unmounted and powered off after each capture.

The E500 log reaches the desktop, unmasks IRQ1/IRQ12 and continues through
input polling and the idle wake path, but records neither a keyboard event nor
a mouse movement; the T23 log records mouse movement. An idle wake alone does
not establish that either input IRQ arrived. A focused E500 diagnostic should
compare BIOS INT 16h AH=09h capability and AH=11h versus legacy AH=01h after
a keypress, and separately sample PIC IRR/ISR for IRQ1 and IRQ12 during key
and mouse activity. The current desktop normally polls enhanced AH=11h/10h;
its AH=01h/00h fallback is enabled only in safe mode. These checks will
distinguish an unsupported BIOS keyboard function from absent controller
IRQs; the keyboard path alone does not explain the mouse symptom. The
[IBM PS/2 BIOS interface reference](https://www.bitsavers.org/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf)
documents the enhanced INT 16h functions.

The installation command, retained for the audit record, was:

```bash
sudo python3 build/hardware/native-memory-20261002/install_memory_map_mode.py
```

The IBM T23 capture has already been saved. Its prepared capture command was:

```bash
sudo python3 scripts/read_physical_memory_map.py --machine ibm-t23
```

The Compaq E500 capture has already been saved. Its capture command was:

```bash
sudo python3 scripts/read_physical_memory_map.py --machine compaq-e500
```

Each boot replaces `MEMMAP.BIN`, so the two captures must be read separately.
The capture script only reads the identified SSD and refuses to overwrite
an existing result. The machine name records the operator's test sequence;
the firmware map itself does not include a machine identifier. Review each
JSON file against the E820 allocation rules in
`docs/native-memory-architecture-2026-10-02.md` before enabling any native
physical-page allocator on either laptop.

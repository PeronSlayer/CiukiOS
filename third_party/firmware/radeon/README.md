# Radeon CP firmware delivery

The two unchanged 2048-byte firmware files are supplied for lead decision
D-F5. The current R100/R200 backend uses MMIO and leaves CP_CSQ_CNTL disabled;
it never loads or executes this firmware. The image installs the files and
notices in `/System/firmware/`.

Source: [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/radeon),
from the lead-provided local primary-source cache. `sources.json` records the
content hashes; `WHENCE` is the exact upstream Radeon CP section, including its
MIT SPDX declaration and AMD copyright. The full upstream WHENCE hash is also
recorded. No CP/DMA support is implied by delivery.

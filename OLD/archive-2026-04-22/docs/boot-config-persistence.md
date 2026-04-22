# Boot Config Persistence v1

## Goal
Persist boot video selection across reboot even when FAT writes are volatile in stage2 runtime.

## Persistence Substrate
Primary persistence is CMOS (RTC NVRAM), with FAT `VMODE.CFG` kept as best-effort mirror.

## Data Model
`bootcfg_data_t` (`boot/proto/bootcfg.h`) uses fixed 24-byte payload:
1. `magic[4]`: `CIUK`
2. `version` (`1`)
3. `flags`
4. `reserved`
5. `mode_id`
6. `width`
7. `height`
8. `crc32` (CRC32 over bytes `0..19`)

Constants:
1. CMOS base index: `0x40`
2. CMOS span: `24` bytes
3. Disabled mode marker: `mode_id = 0xFFFFFFFF`

## Validation Rules
A stored config is considered valid only when:
1. `magic == CIUK`
2. `version == 1`
3. `crc32` matches recomputed checksum
4. `flags` includes enabled bit
5. At least one selector is present (`mode_id` or `width+height`)

Invalid checksum or malformed payload is treated as absent config.

## Loader Precedence
UEFI loader mode selection order:
1. Valid CMOS boot config
2. Valid `VMODE.CFG`
3. Existing policy/default selection

Deterministic marker:
1. `GOP: config source=CMOS ...`
2. `GOP: config source=VMODE.CFG ...`
3. `GOP: config source=POLICY ...`

## Shell Integration (`vmode`)
1. `vmode set ...` and `vmode max` write CMOS boot config first.
2. `VMODE.CFG` is updated as non-fatal mirror.
3. `vmode clear` clears CMOS config and tries to remove mirror file.
4. User-facing text explicitly indicates reboot requirement and source precedence.

## Test Gate
`make test-vmode-persistence` runs a reboot scenario and verifies loader source marker from CMOS plus panic-free runtime markers.

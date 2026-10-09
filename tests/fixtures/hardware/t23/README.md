# T23 firmware replay

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

Distinct E820 maps: 1; distinct video data sets: 2.

The diskseq58 trace has pitches 512 (1024x768x32), 400 (800x600x32)
and 320 (640x480x32); these must be rejected as malformed scanlines.
Later captures report the corrected 4096-byte pitch for mode 0x118.

Raw-file provenance (capture / file / SHA-256):

- `physical-logs-20261007-204040/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq58/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq59/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq59-recheck/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq60/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq60-complete/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq60-final/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq61/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq62/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq63/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq64/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261008-diskseq65/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261009-diskseq32-r10e/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261009-diskseq33-r11/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261009-diskseq34-r12/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261009-diskseq35-r13/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261009-diskseq36-r14/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261009-diskseq66/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261009-diskseq67/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261007-1844/DRIVERS/ACTIVE.CFG`: `488217259eecfed782c11c69d5ce94f04b2b7f35a42bc929e6690362c6bf51b7`
- `physical-logs-20261007-204040/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq58/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq59/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq59-recheck/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq60/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq60-complete/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq60-final/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq61/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq62/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq63/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq64/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq65/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261009-diskseq32-r10e/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261009-diskseq33-r11/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261009-diskseq34-r12/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261009-diskseq35-r13/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261009-diskseq36-r14/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261009-diskseq66/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261009-diskseq67/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261007-1844/SYSTEM/MEMMAP.BIN`: `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`
- `physical-logs-20261008-diskseq58/SYSTEM/VIDEO/VBE.TRC`: `333daa4d2ccfc94018a5b9d8ea125fe046c7c24834ea49143b4f353f4d8d518b`
- `physical-logs-20261008-diskseq58/SYSTEM/VIDEO/DISPLAY.LOG`: `7d68fc3168f4cb25bf40b632a7a940d4a0c0784e6397baa90d4c3b3dde3e6489`
- `physical-logs-20261008-diskseq59/SYSTEM/VIDEO/VBE.TRC`: `b0884d7ae67f2924e7045182bd33dd6a4d72334f1aeb551209f18ccf13388532`
- `physical-logs-20261008-diskseq59/SYSTEM/VIDEO/DISPLAY.LOG`: `8a32cf2f4fd745d14040e09a48fd09ea30c5b0053e06f773327a61999cb7b8cd`
- `physical-logs-20261008-diskseq59-recheck/SYSTEM/VIDEO/VBE.TRC`: `b0884d7ae67f2924e7045182bd33dd6a4d72334f1aeb551209f18ccf13388532`
- `physical-logs-20261008-diskseq59-recheck/SYSTEM/VIDEO/DISPLAY.LOG`: `8a32cf2f4fd745d14040e09a48fd09ea30c5b0053e06f773327a61999cb7b8cd`
- `physical-logs-20261008-diskseq60/SYSTEM/VIDEO/VBE.TRC`: `87115c17746ecfb8726220920c683034905b86c218d6ff846b1ee52913be25f0`
- `physical-logs-20261008-diskseq60/SYSTEM/VIDEO/DISPLAY.LOG`: `60b80180f9e1e4f367e1e29c5eab0b41b7bb2b86c376e9df3119be44900a87b9`
- `physical-logs-20261008-diskseq60-complete/SYSTEM/VIDEO/VBE.TRC`: `87115c17746ecfb8726220920c683034905b86c218d6ff846b1ee52913be25f0`
- `physical-logs-20261008-diskseq60-complete/SYSTEM/VIDEO/DISPLAY.LOG`: `60b80180f9e1e4f367e1e29c5eab0b41b7bb2b86c376e9df3119be44900a87b9`
- `physical-logs-20261008-diskseq60-final/SYSTEM/VIDEO/VBE.TRC`: `87115c17746ecfb8726220920c683034905b86c218d6ff846b1ee52913be25f0`
- `physical-logs-20261008-diskseq60-final/SYSTEM/VIDEO/DISPLAY.LOG`: `60b80180f9e1e4f367e1e29c5eab0b41b7bb2b86c376e9df3119be44900a87b9`
- `physical-logs-20261008-diskseq62/SYSTEM/VIDEO/VBE.TRC`: `87115c17746ecfb8726220920c683034905b86c218d6ff846b1ee52913be25f0`
- `physical-logs-20261008-diskseq62/SYSTEM/VIDEO/DISPLAY.LOG`: `60b80180f9e1e4f367e1e29c5eab0b41b7bb2b86c376e9df3119be44900a87b9`
- `physical-logs-20261008-diskseq63/SYSTEM/VIDEO/VBE.TRC`: `645ee0c992c806e87098d9375fbb2fa8a5ca3cbad1b2943121adbf65be358e56`
- `physical-logs-20261008-diskseq63/SYSTEM/VIDEO/DISPLAY.LOG`: `ef3f36d2c1f3d10e6b70d4cb03e74f03195d40664f720ac05eaa60c47890d9c2`
- `physical-logs-20261008-diskseq64/SYSTEM/VIDEO/VBE.TRC`: `645ee0c992c806e87098d9375fbb2fa8a5ca3cbad1b2943121adbf65be358e56`
- `physical-logs-20261008-diskseq64/SYSTEM/VIDEO/DISPLAY.LOG`: `ef3f36d2c1f3d10e6b70d4cb03e74f03195d40664f720ac05eaa60c47890d9c2`
- `physical-logs-20261008-diskseq65/SYSTEM/VIDEO/VBE.TRC`: `645ee0c992c806e87098d9375fbb2fa8a5ca3cbad1b2943121adbf65be358e56`
- `physical-logs-20261008-diskseq65/SYSTEM/VIDEO/DISPLAY.LOG`: `ef3f36d2c1f3d10e6b70d4cb03e74f03195d40664f720ac05eaa60c47890d9c2`
- `physical-logs-20261009-diskseq32-r10e/SYSTEM/VIDEO/VBE.TRC`: `b90c7f774be62ee1f165dee2f63d2c23ea76945fb01eba130fe11e9ed3fbe4a8`
- `physical-logs-20261009-diskseq32-r10e/SYSTEM/VIDEO/DISPLAY.LOG`: `5ee96acf1d766faefb56c5d90a291d44a204140e910d58885606deafeddcf3ad`
- `physical-logs-20261009-diskseq33-r11/SYSTEM/VIDEO/VBE.TRC`: `b90c7f774be62ee1f165dee2f63d2c23ea76945fb01eba130fe11e9ed3fbe4a8`
- `physical-logs-20261009-diskseq33-r11/SYSTEM/VIDEO/DISPLAY.LOG`: `5ee96acf1d766faefb56c5d90a291d44a204140e910d58885606deafeddcf3ad`
- `physical-logs-20261009-diskseq34-r12/SYSTEM/VIDEO/VBE.TRC`: `b90c7f774be62ee1f165dee2f63d2c23ea76945fb01eba130fe11e9ed3fbe4a8`
- `physical-logs-20261009-diskseq34-r12/SYSTEM/VIDEO/DISPLAY.LOG`: `5ee96acf1d766faefb56c5d90a291d44a204140e910d58885606deafeddcf3ad`
- `physical-logs-20261009-diskseq35-r13/SYSTEM/VIDEO/VBE.TRC`: `b90c7f774be62ee1f165dee2f63d2c23ea76945fb01eba130fe11e9ed3fbe4a8`
- `physical-logs-20261009-diskseq35-r13/SYSTEM/VIDEO/DISPLAY.LOG`: `5ee96acf1d766faefb56c5d90a291d44a204140e910d58885606deafeddcf3ad`
- `physical-logs-20261009-diskseq36-r14/SYSTEM/VIDEO/VBE.TRC`: `b90c7f774be62ee1f165dee2f63d2c23ea76945fb01eba130fe11e9ed3fbe4a8`
- `physical-logs-20261009-diskseq36-r14/SYSTEM/VIDEO/DISPLAY.LOG`: `5ee96acf1d766faefb56c5d90a291d44a204140e910d58885606deafeddcf3ad`
- `physical-logs-20261009-diskseq66/SYSTEM/VIDEO/VBE.TRC`: `645ee0c992c806e87098d9375fbb2fa8a5ca3cbad1b2943121adbf65be358e56`
- `physical-logs-20261009-diskseq66/SYSTEM/VIDEO/DISPLAY.LOG`: `ef3f36d2c1f3d10e6b70d4cb03e74f03195d40664f720ac05eaa60c47890d9c2`
- `physical-logs-20261009-diskseq67/SYSTEM/VIDEO/VBE.TRC`: `b90c7f774be62ee1f165dee2f63d2c23ea76945fb01eba130fe11e9ed3fbe4a8`
- `physical-logs-20261009-diskseq67/SYSTEM/VIDEO/DISPLAY.LOG`: `5ee96acf1d766faefb56c5d90a291d44a204140e910d58885606deafeddcf3ad`

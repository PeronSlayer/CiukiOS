# F0-54 loader byte budget

Record of directive `f0-54` (2026-10-21): free at least 512 bytes inside the
fixed 48-sector (24,576-byte) extent of `CIUKLDR` without behaviour change.
Base: `main` at `4140362b`. Host evidence only; no image build, QEMU or
hardware run belongs to this record.

## Measurement

`python3 scripts/loader_budget.py` assembles `src/boot/ciukldr.asm` alone with
the release version define (`--bare` uses the `unknown` fallback), reads the
NASM listing and classifies every emitted byte: header (16), code, tables
(non-text `db`/`dw`/`dd`/`dq`), strings (quoted data), scratch (initialized
`times` buffers), padding (alignment and the free tail) and the 32-byte
`CLDRPRG1` trail. The listing must cover the whole 24,576 bytes without gaps,
or the tool fails. `free` is the tail before the trail, that is the value
`times 24544-($-$$)` pads; a negative value is the `TIMES value ... is
negative` overflow that stopped the f1-88, f1-87 and f5-15A2 merges.
`test_boot_modes.LoaderTests.test_loader_image_keeps_its_fixed_size_and_hint`
prints it for the bare, `0.9.0 pre-alpha` and current release versions and
asserts `free >= 512`.

| Build | Version define | Code | Tables | Strings | Scratch | Padding | Free |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Before (`4140362b`) | `unknown` | 8,069 | 252 | 1,663 | 14,520 | 24 | **4** |
| Before (`4140362b`) | `0.9.2 pre-alpha` | 8,069 | 252 | 1,671 | 14,520 | 16 | **4** |
| Parser change only | `unknown` | 7,714 | 266 | 1,738 | 14,520 | 290 | 276 |
| Parser change only | `0.9.2 pre-alpha` | 7,714 | 266 | 1,746 | 14,520 | 282 | 260 |
| After | `unknown` | 7,714 | 266 | 1,738 | 13,496 | 1,314 | **1,300** |
| After | `0.9.2 pre-alpha` | 7,714 | 266 | 1,746 | 13,496 | 1,306 | **1,284** |

`0.9.0 pre-alpha` measures like `0.9.2 pre-alpha`. The `align 16` before
`boot_info` absorbs the 8-byte longer release title, so "before" is 4 bytes
for every define. The loader BSS (`loader_bss_end` = `0x6841`, `loader_end`
= `0x26841`) and the page-rounded reservation are unchanged.

## Changes

1. **Selector and option parser** (`src/boot/ciukldr/menu.inc`, -355 code
   bytes, +14 table bytes, +75 string bytes):
   - one matcher, `token_match`, compares NUL-terminated tokens bounded by
     `[selector_end]`; `token_hex` matches a prefix and exactly N hex digits
     (the former `hex_digit` routine is folded into it with the same
     accepted set: `0-9`, `a-f`, `A-F`);
   - the phase digit indexes one word table (`probe_tables`) instead of the
     seven compare/branch pairs;
   - the cfg-only sweep is recognised by the matched entry being `sweep`
     instead of comparing five per-table labels (every `sweep` entry was one
     of those labels, and no table has another name starting with `sweep`);
   - the `run=`, `step=`, `state=`, `title=`, `platform=e500`, `safe=1` and
     `server=` suffixes and the BOOT.CFG options (`safe=0|1`, `serial=0|1`,
     `accel=0`, `mode=0x<4 hex>`) go through the same matcher; an option is
     accepted only when the token is followed by space, CR, LF or the end of
     the file, which is exactly the former split-then-compare rule.
2. **ELF program-header scratch** (`src/boot/ciukldr.asm`, -1,024 scratch
   bytes): `ph_table` is placed inside `raw_map` (`ph_table equ raw_map`,
   with an assembly-time check that 32 × 32 bytes fit the 128 × 24-byte
   buffer). `raw_map` is read and written only by `memory_collect` and
   `map_normalize`, which run before A20, the FAT mount and the menu; every
   later RAM check uses the normalized `boot_info.e820` array. `elf_load`
   runs last before the handoff and `file_read` fills all `ph_count * 32`
   bytes (validated against the file size) before the first program header
   is read; entries beyond `ph_count` are never read. The kernel cannot
   address `raw_map` (no pointer in `boot_info`) and reclaims the loader
   extent after the handoff.

Not changed: probe names, their order, every accepted selector and option,
`L:` record text, the menu title, hint, prompts, timing and deadlines, and
the source layout of the probe tables.

## Constraints found in existing tests

The directive's preferred compact table encoding (length-prefixed names,
shared `w32-`/`vm-` prefixes) was not applied: existing tests parse the
source text of the tables and would have to change.

- `tests/host/win32/test_probe_registry.py` requires a one-line
  `f5_probe_names db 'name',0,...,0,0` list equal to the kernel's `f5_names`.
- `tests/host/test_f3_workloads.py` requires the literals `'quake',0`,
  `'dos4gw',0`, `'doom',0`, `'doom-sound',0`, `'wolf3d',0` and
  `'vm-starve',0`.
- `tests/host/test_runner.py` (F2 evidence) reads the quoted F2 names between
  `f2_probe_names:` and `platform_suffix`; `tests/host/win32/gl/test_gl.py`
  counts `'w32-gl'`; `scripts/test/probe_tables.py` reads NUL-separated `db`
  lists for `test_runner_selectors`.
- `tests/host/test_desktop_gate.py` requires the `desktop_suffix` and
  `standin_suffix` definitions, `cmp bx, f2_crash_name`,
  `cmp byte [selector_fw], 1` and `cmp cx, 64`.

A `w32-` prefix encoding would save about 50 bytes net; the parser change
and the scratch placement already give 1,284 bytes.

## Verification

- New `LoaderParserTests` in `tests/host/test_boot_modes.py` run the
  assembled `selector_validate`, `selector_accept`, `config_read` and the
  first part of `elf_load` in `tests/host/loader_parser_machine.py`, a
  bounded executor over `ndisasm` output (unsupported instructions fail
  closed), and compare them with the existing models: `scripts/test/run.py`
  `selector` (menu and fw_cfg), `scripts/test/physical.py` `cfg_selector`
  (BOOT.CFG) and `loader_model.boot_options`. They check acceptance, the
  force, safe and sweep flags, the copied request, the option flags and
  mode, and that `elf_load` fills the shared scratch before the first
  header validation without touching `boot_info.e820`.
- One-off differential run (not committed) of the `4140362b` parser against
  the new one in the same executor: 14,943 selector strings (every table
  name with each prefix, run, suffix, step, state and title form, plus
  6,000 seeded random edits) for each of the menu, fw_cfg and BOOT.CFG
  sources, 44,829 runs with 1,005 accepted: accept/reject, returned SI/CX
  and the force, safe and sweep flags matched in every run. 5,269 BOOT.CFG
  texts (single options, separators and 6,000 seeded random combinations,
  including `probe=` lines) with three initial flag values, 15,807 runs
  with 825 non-fatal: fatal/non-fatal outcome, flags, preferred mode and
  the copied request matched in every run.
- Host suites: `test_boot_modes`, `test_runner_selectors`,
  `test_desktop_gate`, `test_loader_menu`, `test_loader_model`,
  `test_f3_workloads`, `test_version`, `test_hardware_check`,
  `test_runner.F2EvidenceTests`, `win32/test_probe_registry`,
  `reliability/test_loader_progress`, `tests/loader_stub`, and the whole
  `scripts/test/host_kernel_tests.sh` (exit 0).

One observable-free difference: on a rejected fw_cfg F3 selector such as
`f3:vm-boot run=12345678 platform=e500 safe=1`, the new parser sets
`selector_force` before rejecting the trailing text, as the old parser
already did for the same text on F0-F2 probes. `selector_force` is cleared
at every `selector_validate` entry and read only by `selector_accept`, which
runs only after a successful validation.

## Sources

- NASM manual, [chapter 3](https://www.nasm.us/doc/nasm03.html): `TIMES`,
  `DB` string data and character constants (`'swee'` as a dword compares the
  bytes `s w e e` in memory order, little-endian).
- Intel 64 and IA-32 Architectures Software Developer's Manual, volume 2
  ([index](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)):
  `CMP` leaves CF clear on equality, `TEST`/`OR` clear CF, `INC` keeps CF,
  `LODSB` advances SI with DF clear (the loader executes `cld` at entry),
  `LOOP` decrements CX without changing flags.
- [ACPI 6.5, System Address Map Interfaces](https://uefi.org/specs/ACPI/6.5/15_System_Address_Map_Interfaces.html):
  the E820 descriptor buffer is caller-owned; the loader keeps only the
  normalized copy.
- [System V gABI, program header](https://gabi.xinuos.com/elf/07-pheader.html):
  `e_phnum` × `e_phentsize` bytes at `e_phoff`; the loader requires
  `e_phentsize` = 32 and 1..32 entries.

## Merge with main (`aac5cb00`)

Main had meanwhile added the f3-55 `f3:dos-conformance` selector
(`guest=freedos`, `guest=ciukidos`, `guest=ciukidos kernel=unified`, every
source, no other suffix) and shared/shortened loader strings
(`minimum_prefix`, `source_prefix`, the `fw_cfg`/`cfg`/`newline` tail).
The merge keeps main's strings and expresses the DOS grammar with the
shared matcher: after `run=`, the `dos-conformance` entry accepts exactly
` guest=freedos`, ` guest=ciukidos` or ` guest=ciukidos` + ` kernel=unified`
up to the selector end. `probe_tables` follows `chain_probe_names`, because
`tests/host/dos/test_protocol.py` assembles the data between
`chain_probe_names db` and `config_read:` together with `selector_validate`.

| Build | Code | Tables | Strings | Scratch | Padding | Free |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| main `aac5cb00`, `0.9.2 pre-alpha` | 8,130 | 252 | 1,604 | 14,520 | 22 | 7 |
| Merged, `0.9.2 pre-alpha` | 7,773 | 266 | 1,682 | 13,496 | 1,311 | **1,303** |

The bare and `0.9.0 pre-alpha` assemblies also measure 1,303 free bytes.
A one-off differential run of main's parser against the merged one (same
executor, the earlier corpus plus 16 DOS suffix forms on every phase)
matched in all 60,102 selector runs (989 accepted) and all 15,795 BOOT.CFG
runs (828 non-fatal).

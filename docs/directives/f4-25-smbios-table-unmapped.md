# f4-25 — SMBIOS table unmapped on the T23: the entry point is found but the structure table lives at 0x1FF7C000

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f4-25` from `main`. Host-testable; the lead tests on the T23.

## Evidence (T23, image ce2e6ce9, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/ce2e6ce9/disk-log/BOOT.LOG.clean`, lines 54-58)

```
[smbios] entry=unmapped_table address=000f7350 table=1ff7c000 bytes=1717
[smbios] version=2.31 vendor="" product="" version="" type1=0
[smbios] bios vendor="" version="" type0=0
[ec] thinkpad hint=0
[ec] thinkpad gate=no_smbios
```

f4-21's reader found the `_SM_` entry at 0xF7350 (SMBIOS 2.31) but refuses to dereference the structure table at physical 0x1FF7C000 (1717 bytes, in the firmware-reserved top of the 512 MiB RAM) because that page is not in the kernel's direct map. So the EC driver stays off and the volume keys do nothing.

## Task

1. Map the table range temporarily (the kernel has a physical-window/`ua_map`-style facility; use the existing one, read-only, page-aligned, bounded to `bytes`), parse, unmap; log the same records with the real strings. Verify the range is reserved in E820 (expected `reserved`) and refuse ranges inside usable RAM that the allocator owns unless already direct-mapped.
2. Host test with a table outside the direct map.

## Acceptance

Audio host suite green; on the T23 `[smbios] vendor="IBM" product="2647..."` and `[ec] thinkpad volume=` records, keys moving the master. Files: `thinkpad_ec.c`/SMBIOS reader, mapping helper call, tests. Not `savage2d.c` (f4-26) or USB.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research the references first where named (`--search`), record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f4-25-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify.

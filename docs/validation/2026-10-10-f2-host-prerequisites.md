# F2 host prerequisite fixture corrections

The payload-manifest fixture must supply the kernel build's clock record.
`scripts/build_kernel.py` records `utc_epoch` (the current `int(time.time())`,
or an explicitly supplied `SOURCE_DATE_EPOCH`) and the linked kernel's SHA-256
in `build-clock.json`. `scripts/build_image.py` verifies that hash and copies
the recorded epoch into `build-manifest.json`; it must not invent a replacement
epoch when the record is absent.

[Python's time documentation](https://docs.python.org/3/library/time.html#time.time)
defines `time.time()` as seconds since the UTC Unix epoch. The host fixture now
records the current epoch and its fixture kernel's actual hash in the same JSON
format, then checks both values in the manifest.

The map test expects the registrations actually present in the linked sources:
`crash-isolation`, `libc-smoke`, `fd-table`, `elf-load`, `spawn-wait`, `mmap`,
`threads-wait`, `signals-fault`. This is linker source order, rather than the
acceptance table's presentation order. `app-gate` is named in
[`f2-acceptance.md`](../design/f2-acceptance.md) and the selector but has no
`CIUKI_F2_PROBE` registration yet. The test retains the registration-size and
individual-section bounds within `.rodata`.

Validation in this worktree:

- `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host -v`:
  88 tests, OK with four prerequisite skips (three require the desktop build;
  one requires the Lua SDK build). Both corrected tests and both kernel map
  checks passed after the kernel build.
- `python3 scripts/build_kernel.py`: passed, including the FPU/SIMD audit;
  produced `build/f0/VMM.ELF`, `VMM.map` and `build-clock.json`.
- Logs and temporary artifacts were confined to this worktree's `build/`.
  No QEMU or image build was run.

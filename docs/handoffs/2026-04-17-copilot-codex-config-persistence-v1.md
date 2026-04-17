# Handoff - 2026-04-17 - copilot-codex-config-persistence-v1

## 1) Context and Goal
Implement persistent boot video configuration that survives reboot by using CMOS as primary runtime store, with FAT `VMODE.CFG` as best-effort mirror.

## 2) Changed Files
1. `boot/proto/bootcfg.h`
2. `boot/uefi-loader/loader.c`
3. `stage2/src/shell.c`
4. `scripts/test_vmode_persistence_reboot.sh`
5. `Makefile`
6. `docs/boot-config-persistence.md`
7. `Roadmap.md`
8. `README.md`

## 3) Persisted Format (v1)
Stored payload (`bootcfg_data_t`, 24 bytes):
1. `magic[4] = CIUK`
2. `version = 1`
3. `flags`
4. `reserved`
5. `mode_id`
6. `width`
7. `height`
8. `crc32`

Validation:
1. magic/version match
2. CRC32 over bytes `0..19` matches `crc32`
3. enabled flag present
4. at least one selector present (`mode_id` or `width+height`)

CMOS location:
1. base index `0x40`
2. span `24` bytes

## 4) Loader Selection Precedence
Implemented order:
1. valid CMOS bootcfg
2. valid `VMODE.CFG`
3. policy/default fallback

Deterministic markers:
1. `GOP: config source=CMOS ...`
2. `GOP: config source=VMODE.CFG ...`
3. `GOP: config source=POLICY ...`

## 5) `vmode` Integration
1. `vmode set` and `vmode max` now persist to CMOS first.
2. `VMODE.CFG` is written as non-fatal mirror.
3. `vmode clear` clears CMOS + mirror delete best-effort.
4. User messages explicitly mention reboot requirement and source precedence.

## 6) Validation Executed
1. `make all` -> PASS
2. `make test-stage2` -> FAIL (host serial/debug capture unavailable)
3. `make test-video-mode` -> FAIL (required runtime markers not captured in host log)
4. `make test-vmode-persistence` -> FAIL (required runtime markers not captured in host log)

## 7) Risks / Next Step
1. Runtime test failures observed are infra-level log-capture issues on this host, not compile-time failures.
2. Next step: stabilize QEMU serial/debugcon capture path, then re-run the three runtime gates for final green closure.

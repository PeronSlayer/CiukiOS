# Directive f2-07: Lua 5.4.8 port, the F2 test supplement and the image payloads

- **Step:** F2. **Contracts:** `posix-subset.md` ("Application gate and
  graphical stretch", SDK), `f2-acceptance.md` ("Lua application evidence",
  T1 payload rules: every payload listed by path and hash in
  `build-manifest.json`; `/bin`, `/tmp`, `/home`, `/system` on the boot
  volume), `boot-memory.md` (image layout).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Prerequisite on `main`:** f2-06 (SDK). The archives are already pinned
  in `config/sdk-pins.json` and present in
  `/home/peronslayer/Desktop/CiukiOS/build/downloads/newlib/`
  (`lua-5.4.8.tar.gz`, `lua-5.4.8-tests.tar.gz`); the build refuses digest
  mismatches and never downloads.
- **Worktree:** `wt/f2-lua`. Files: new `apps/lua/` (`build_lua.py`,
  `patches/` if any — none expected —, `ciuki-f2.lua`, `README.md`),
  `scripts/build_image.py` (populate `/bin/lua`, `/system/tests/lua-5.4.8-tests/`
  from the unmodified test archive, `/tmp`, `/home`, the SDK test programs
  `/bin/hello` and `/bin/libc_smoke`, and record every payload path and
  SHA-256 in `build-manifest.json`; keep the 512 MiB FAT32 layout and the
  T1 checks), `Makefile` (`build-full` builds the SDK and apps before the
  image when their inputs changed; `lua` target), `tests/host/test_image_payloads.py`
  (or an addition to an existing host test), `docs/sdk.md` (how to build
  and run Lua), `config/sdk-pins.json` only if a field is missing.

## What to build

1. **Lua build**: `apps/lua/build_lua.py` extracts the pinned source into
   `build/apps/lua/`, builds `lua` and `luac` with `ciuki-cc` using the
   portable configuration (`LUA_USE_C89` off, no `LUA_USE_LINUX`, no
   readline, no `dlopen`, `LUA_32BITS` off: 64-bit integers and doubles,
   `LUA_COMPAT_5_3` as upstream default), verifies the ELF with the SDK's
   checker (static, three LOADs, no SSE/MMX, no undefined symbols), and
   records source/tests archive hashes and the ELF hash. Upstream sources
   are not modified; if a build fix is unavoidable it goes into
   `apps/lua/patches/` with a hash and a reason.
2. **Test supplement** `apps/lua/ciuki-f2.lua`: the four cases of
   `f2-acceptance.md` (`file-roundtrip` 65,536 bytes with byte i = i mod 251,
   rewrite of bytes 8192–12287 as 255 − original, close/reopen/compare,
   rename/remove; `allocation` 100 cycles of 4,096 live 128-byte strings
   with full collection and reference checks; `time-utc` round trips for
   2000-02-29 and 2040-01-01 through `os.time`/`os.date` plus non-negative
   `os.clock`; `console` with known ordered stdout/stderr lines and exit 0),
   each printing `case=<name> ok=1|0 …` lines the controller frames, and
   exiting non-zero on any failure.
3. **Image payloads**: `scripts/build_image.py` creates `/bin`, `/tmp`,
   `/home`, `/system/tests/lua-5.4.8-tests` (the complete unmodified test
   directory from the archive), copies `lua`, `hello`, `libc_smoke` into
   `/bin`, and writes `payloads: [{path, sha256, size}]` into
   `build-manifest.json`; T1 verifies each file on the image by reading it
   back with `mtools` and comparing hashes; the image stays 512 MiB.
4. **Host checks**: a host-side run of the Lua portable suite with a
   *host* Lua build of the same sources is useful to validate the archive
   and the `_U` procedure, but it is explicitly **not** gate evidence
   (contract): record it as `host-reference` only.

## Acceptance by the lead

Diff review; `make build-full` from a clean tree builds SDK, Lua and the
image in the capped scope; T1 payload checks pass; the F0 and F1 suites
still pass on the new image (payloads do not change kernel behaviour).
Reply with: files, the ELF sizes, the payload list, host-reference suite
result, and any contract problem found.

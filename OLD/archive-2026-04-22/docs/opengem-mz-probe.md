# OpenGEM MZ deep-header probe (OPENGEM-015)

## Context
OPENGEM-013 confirmed via a 2-byte peek that gem.exe is a real MZ binary. OPENGEM-015 parses the full 28-byte MZ header on the already-staged buffer and surfaces every field a 16-bit execution layer will need: entry CS:IP, stack SS:SP, minimum/maximum allocation, relocation table offset, and a computed load size. It also publishes an explicit viability verdict ("runnable-real-mode" vs "requires-extender") so the DPMI/v8086 requirement becomes a first-class observable instead of a `shell_run`-side rejection string.

This is pure observability — no execution change. MZ still goes through `shell_run()` and gets the historical `[dosrun] mz dispatch=pending reason=16bit`.

## Marker set (frozen, append-only)
```
OpenGEM: mz-probe begin path=<p> size=0x<hex32>
OpenGEM: mz-probe signature=<MZ|ZM|none> status=<ok|too-small|not-mz>
OpenGEM: mz-probe header e_cblp=0x<h16> e_cp=0x<h16> e_crlc=0x<h16> e_cparhdr=0x<h16>
OpenGEM: mz-probe alloc e_minalloc=0x<h16> e_maxalloc=0x<h16>
OpenGEM: mz-probe stack e_ss=0x<h16> e_sp=0x<h16>
OpenGEM: mz-probe entry e_cs=0x<h16> e_ip=0x<h16>
OpenGEM: mz-probe reloc e_lfarlc=0x<h16> e_ovno=0x<h16>
OpenGEM: mz-probe layout load_bytes=0x<h32> header_bytes=0x<h32>
OpenGEM: mz-probe viability=<runnable-real-mode|requires-extender|malformed|skipped-non-mz> reason=<token>
OpenGEM: mz-probe complete
```

## Viability tokens (stable)
| Viability | Meaning |
|-----------|---------|
| `runnable-real-mode` | Small MZ that fits the real-mode window (load ≤ 640 KiB and not pinned to e_maxalloc=0xFFFF with load > 64 KiB). |
| `requires-extender` | Needs DPMI / DOS4GW (gem.exe lands here). |
| `malformed` | Header inconsistent (e.g. e_cparhdr==0 or e_cp==0). |
| `skipped-non-mz` | Buffer does not start with MZ/ZM. |

## Reason tokens (stable, disjoint from OPENGEM-012/013/014)
| Reason | Condition |
|--------|-----------|
| `mz-v8086-candidate` | viability=runnable-real-mode |
| `mz-load-exceeds-real-mode` | load_bytes > 0xA0000 |
| `mz-max-alloc-64k` | e_maxalloc==0xFFFF and load_bytes > 0x10000 |
| `mz-header-too-small` | preload_size < 0x1C |
| `mz-header-malformed` | e_cparhdr==0 or e_cp==0 |
| `mz-non-mz-skipped` | signature mismatch |
| `mz-no-buffer` | no path / no preload |

## Load-size formula
```
file_bytes  = e_cp * 512
if (e_cblp != 0) file_bytes -= (512 - e_cblp)
header_bytes = e_cparhdr * 16
load_bytes   = file_bytes - header_bytes
```

## Invocation
Gated on `classify_label == "mz"`. Sequence inside `shell_run_opengem_interactive`:
```
preload_absolute()      (OPENGEM-013, extended in OPENGEM-014)
  -> stage2_opengem_mz_probe()      (this phase, only for MZ)
  -> stage2_opengem_dispatch_native() (OPENGEM-014 — returns 0 for MZ)
  -> shell_run()                     (historical path, still owns MZ)
```

## Files touched
- `stage2/src/shell.c` — new `stage2_opengem_mz_probe()` + gated call after preload.
- `scripts/test_opengem_mz_probe.sh` — new gate (41 OK / 0 FAIL).
- `Makefile` — target `test-opengem-mz-probe`.

## Gate assertions (static)
- Sentinel `OPENGEM-015` and helper presence.
- All 10 marker lines present.
- All 12 header fields surfaced (`e_cblp` … `e_ovno`).
- All 4 viability labels + 7 reason tokens + 2 non-"ok" status labels.
- Probe gated on `classify_label==mz`.
- Ordering `preload → mz_probe → dispatch_native`.
- Internal marker ordering (serial_write lines only), all 10 in stated sequence.
- Makefile target declared.

## Runtime (opt-in via `CIUKIOS_OPENGEM_BOOT_LOG`)
- `mz-probe begin path=… size=0x<8 hex digits>` well-formed.
- `mz-probe signature=<MZ|ZM|none> status=<ok|too-small|not-mz>`.
- `mz-probe viability=<…> reason=<kebab>`.

## Risks
- Relocation-table validity is not cross-checked — only e_crlc and e_lfarlc are reported. A malicious MZ with out-of-range e_lfarlc would not be caught until the real loader.
- The viability heuristic is conservative: anything with e_maxalloc==0xFFFF and load > 64K is flagged as needs-extender, which is correct for almost all real-world DOS apps (gem.exe included) but would misclassify tiny tools that merely happen to carry default linker settings. Acceptable for observability.
- No execution change — any downstream assumption that `mz-probe viability=runnable-real-mode` implies CiukiOS can actually run it is WRONG until OPENGEM-016+ delivers the 16-bit execution layer.

## Next step
- OPENGEM-016 (architectural multi-session): pick a 16-bit execution strategy. Two plausible designs:
  1. **v8086 monitor in stage2** — set up a v8086 task in long-mode's compatibility chain, vector INT 21h/10h/16h/33h through the existing CiukiOS services layer. Heavyweight (needs GDT tricks, full trap handler, segmentation fix-ups).
  2. **DPMI server on top of DOS4GW-compatible extender** — import the existing `m6_dpmi_*` smoke scaffolding and build a minimal server; MZ files are re-hosted as DPMI clients. Lighter for software that already targets DOS4GW; useless for pure 16-bit like gem.exe.
- Either path is a multi-phase effort; OPENGEM-016 itself would be a design document + a no-code kickoff, not a single-session implementation.

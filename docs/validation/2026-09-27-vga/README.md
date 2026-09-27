# VGA session validation — 27 September 2026

Reports copied from the `build/full/vga-video-2026-09-27/` runs. Screenshots,
physical-memory dumps, disk copies and serial logs remain in ignored `build/`
under the paths each report names. All runs use QEMU `-cpu pentium3 -m 128`
with KVM and say nothing about physical hardware. See
[the video record](../../vm-video-session-2026-09-27.md).

| File | Run | Outcome |
| --- | --- | --- |
| `vga-session-acceptance.json` | `full4`: VGASEM semantics, FIRE, Wolf3D, VGASEM cleanup, Costa | PASS |
| `vga-dos-window.json` | `winrun11`: native desktop DOS window, focus/redraw/minimize, FIRE in window | PASS |
| `legacy-v86-gate.json` | `legacy5`: original V86 session gate with the video-track module | PASS |
| `legacy-dpmi-gate.json` | `legacy-dpmi6`: DPMI video gate with the video-track module and HDPMI `72d7adce…` | PASS |
| `failure-legacy-gate-before-fixture-update.json` | Old fixture expected RAM-alias A000 in text mode | FAIL (preserved) |
| `failure-dpmi-pm-page-fault-before-bridge.json` | PM #PF on the guarded aperture before the 0203h/0301h bridge | FAIL (preserved) |

Binaries used for the passing runs (SHA-256). Each report's `inputs` or
`fixture_sha256` map is authoritative; loose files in the parent build directory
may come from a different assembly invocation.

| Binary | SHA-256 |
| --- | --- |
| `CVSESSION.DLL` | `5e584db0b084677bb8f05bdff5ea1e5a816b0e92e2101410d9b3358deefbd133` |
| `SHELL.COM` (private window image, 60,880 bytes) | `1edc72994b085161bd99750d989b9a2899a0c15726316d9687c8223636a477e7` |
| `DOSWIN.DRV` (34,096 bytes) | `9be2d3376c9adeb3b46e624e24cedd486f8e901c3c98d8ebb62025d9d1dac677` |
| `VGAHOST.COM` (`full4`, `vga-session-acceptance.json`) | `4ac9723c9735672b393caccb17aebe3d78cae0fe80d38eb64706e29543a3c24f` |
| `VGASEM.COM` (`full4`, `vga-session-acceptance.json`) | `bee52ec3c309dfa9ef4629f48de27b706c01937fd4c99a211e1e130b07279a19` |
| `VGASEMW.COM` (`winrun11`, assembled with `HOLD_ALWAYS`) | `d705de5ceb634ff2bf0f5dbca874e38f0a6e7ce98a92dd0e084ef82e1dadf37e` |

The previously listed `VGAHOST.COM` hash `bb71d517…` and `VGASEM.COM` hash
`8c663ba2…` belong to loose files under `build/full/vga-video-2026-09-27/`;
they are not the fixtures recorded by `full4`. None of the reports was changed
to relabel those inputs. All four passing JSON files match the original local
report bytes. These runs predate later scheduler reentry and guest-IF fixes;
see [the integration audit](../../vm-integration-audit-2026-09-27.md).

Host-side checks run with the same sources: `scripts/test_virtual_vga.py`
(168,060 assertions) and `scripts/test_vga_x86.py --cases 20000` (Unicorn
differential, 16- and 32-bit), both PASS.

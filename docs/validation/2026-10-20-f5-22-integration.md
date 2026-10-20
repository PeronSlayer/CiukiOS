# F5-22 integration corrections

Research and implementation decisions for directive
`docs/directives/f5-22-integration-fixes-2026-10-20.md`.

- [Tango upstream releases](https://tango.freedesktop.org/releases/) provide the
  pinned 0.8.90 family. Reuse `media-optical` from the existing deterministic
  store at 16/22/32/48 pixels for CD Player; no new artwork or store conversion.
- [Microsoft MCI_STATUS](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci-status)
  distinguishes device readiness, media presence and track type. Preserve
  f5-12B2's per-slot `MEDIA_CD_STATE` polling for audio-disc AutoPlay; model an
  absent drive explicitly in the shell harness, including drive sentinel 26.
- [SFF-8020i revision 2.6](https://www.jmargolin.com/project/SFF8020i.pdf),
  sections 8.2 and 10.8.6.4, distinguishes data/audio/mixed discs and CD-DA
  read capabilities. Keep the lead's split media and CD guest cases, with the
  optical fixture attached only to the CD case. This is authored host evidence,
  not guest or physical audio qualification.
- [LSB IA32 data representation](https://refspecs.linuxfoundation.org/LSB_1.1.0/IA32/spec/datarepresentation.html)
  delegates aggregate layout to the Intel386 System V ABI. Check every new CD
  record and constant against independent literal expectations, using the
  existing i686 object dump and host `-m32` oracle.

F5-22 explicitly requires `/WINDOWS/CDPLAYER.EXE`. Keep the native `/Programs`
image inventory unchanged. Extend the existing shell `programs[]` catalogue
and Accessories loop with a system PE target, and dispatch through the existing
Win32 launch-record callback (also used by shortcuts and AutoPlay). Update
AutoPlay to the same target. No native ELF admission change is needed.

The production selector tables remain authoritative. Remove model-only
`w32-fixtures` and `w32-kernel32`; all three pending probes (including
`w32-restart`) belong to f5-21's guest observer integration.

Verification commands and outcomes are recorded in
`build/codex/f5-22-report.md`; the lead owns broader suites and image evidence.

## Host evidence

The eight individual failing checks pass. The named desktop module passes
(27 tests, three pre-existing missing-build skips), CiukNote passes (5), icons
pass (12), kernel core passes (5; 62,995 production checks), probe registry
passes (5), and selector/desktop gate passes (17). The canonical target ABI
dump and all 1,496 literal host `-m32` compile-time checks pass. The unchanged
native runtime oracle prints all 1,496 checks passing before LeakSanitizer's
sandbox ptrace failure at exit. The complete kernel script and five media
module cases likewise cannot complete LeakSanitizer in this sandbox; no leak
setting was overridden.

The one authorized app-build invocation compiled SDK, Lua, desktop and TinyCC
but stopped at TinyCC's stale generated API-reference guard. Regenerated
`docs/developer-api.md`; its changes match the public CD declaration formatting.
The lead must rerun the app build and the blocked runtime checks. Exact
commands, exit statuses, skips and remaining evidence are in the report.

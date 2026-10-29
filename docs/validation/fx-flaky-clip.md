# Clipboard host fixture: retired global-block address reuse

Research and implementation: 2026-10-29.

The production-seam clipboard assertion compared a reconstructed
`CF_METAFILEPICT` global-memory handle against the handle freed during
`u32_clip_render_all`. The host fixture implements `GlobalAlloc` with
`mmap(MAP_32BIT)` and `GlobalFree` with `munmap`; its handles are addresses,
not the generational DWORD handles used by the production GDI object table.
The next mapping can reuse the freed address. Distinct lifetimes do not
require distinct numeric global-memory handles.

Sources and decision:

- [Microsoft GetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclipboarddata):
  success returns a clipboard object handle; failure returns NULL. Extended
  error information describes failure, not a successful call. There is no
  requirement that a reconstructed object have a different numeric handle
  from an already freed object.
- [Microsoft Clipboard Operations](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-operations):
  clipboard ownership includes freeing associated GDI objects and global
  memory. Verify reconstruction, content and disposal, rather than uniqueness
  across retired global-memory allocations.
- [Microsoft Clipboard Formats](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-formats):
  palette and metafile clipboard formats have specific object representations.
  This test explicitly publishes all three formats, so its failing assertion
  is about reconstruction and lifetime, not conversion from a missing format.
- [Linux mmap/munmap manual](https://man7.org/linux/man-pages/man2/mmap.2.html):
  the kernel chooses an address for a NULL mapping hint; unmapping releases
  the old mapping. The host adapter cannot promise never to reuse it.

The fixture now deliberately recycles the retired metafile global block once.
It poisons the released payload, accounts for its logical free/allocation,
then supplies the same block to the reconstruction's `GlobalAlloc`. Other
allocations retain the existing mmap adapter. This avoids both ASLR dependence
and any fixed mapping over a potentially live allocation. The test checks the
reuse hook was consumed, checks the palette entries and picture dimensions,
and compares the full WMF and EMF byte streams. Existing generational GDI
handle and clipboard-empty disposal assertions remain.

Restoring the old `got_meta != mfp` condition with this deterministic reuse
aborts (-6). The fixed assertion accepts the reused address and verifies its
reconstructed contents. A successful unmodified baseline fixture also retained
last-error 120: the number printed by the combined assertion was stale and
was not proof that GetClipboardData failed. No production-code or timeout
change is warranted.

The implementer report at `build/codex/fx-flaky-clip-report.md` records exact
host verification commands and results, including contention and full-suite
results. No guest/image/hardware evidence is claimed.

Final host verification: the named DirectDraw test passes, and 20 consecutive
executions of its sanitized production USER/GDI/DirectDraw fixture pass under
two busy loops sharing the same two CPUs at nice 19. The old inequality
reliably aborts under the same contention when address reuse is forced.

The full Win32 discovery run completed 427 tests in 1212.357 seconds:
45 failures, 23 errors, 87 skips. The named DirectDraw seam test and standard
and GL-enabled production USER fixtures pass. Of the failures, 44 report
LeakSanitizer's fatal thread-inspection restriction and one is i386 SIGSYS.
Errors comprise 21 installer JSON decode errors (the companion parser test
reports the LeakSanitizer restriction), one printing process failing after a
PASS with LeakSanitizer's fatal error, and one local Wine build-record
`KeyError: 'dplay.dll'`. No sanitizer setting was changed. Full-suite green
qualification remains for the lead; these results do not claim all other
failures were established as pre-existing.

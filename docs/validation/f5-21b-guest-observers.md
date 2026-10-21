# F5 core guest observer implementation

This record covers the f5-21b implementation, not guest qualification. Host
predicate dry-runs test the publisher and rejection paths; they do not execute
PE instructions. The lead owns image builds and guest evidence.

## Primary-source research and implementation decisions

- [Microsoft VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc):
  reserve/commit at an aligned explicit address, check the exact return value,
  and use a zero-initialized page. The actual loader/import model must prove
  every fixture page free, rather than relying on an address-space guess.
- [Microsoft RegFlushKey](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regflushkey):
  closing/reopening a handle is not a service restart or crash recovery test.
  A successful query alone cannot qualify those two suite predicates.
- [Microsoft DirectDraw Restore](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-restore):
  Restore reallocates lost storage, without restoring its bitmap contents.
  Count a loss/restore cycle only after observing SURFACELOST, then successful
  Restore; system-memory retention needs a separate content comparison.
- [Microsoft waveOutGetPosition](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutgetposition):
  check the returned time format. Queue submission does not prove heard PCM;
  callbacks and consumed position must be observed separately.
- [Microsoft BitBlt](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt)
  and [CombineRgn](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-combinergn):
  verify pixel truth tables and region membership, not just successful calls.
- [Microsoft SendMessage](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessage):
  verify the procedure's result and dispatch count for synchronous sends.
- [Microsoft DirectInput SetDataFormat](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee417925(v=vs.85)):
  set the format before acquisition; test acquired-format rejection separately.
- [Microsoft RaiseException](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raiseexception):
  explicitly raised exceptions are software exceptions. Seven different
  reported exception codes must not be described as seven hardware fault paths.
  Accordingly, the crash fixture uses actual instructions for AV, integer
  divide, CLI, INT3, UD2 and x87 divide, and successive stack-page touches for
  guard growth/exhaustion. The eighth child faults with a malformed ESP.
  It checks matching PID/code records and eight stack words in complete files.
  See [Intel SDM, volume 1, x87 exceptions](https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf)
  and [Microsoft guard pages](https://learn.microsoft.com/en-us/windows/win32/memory/creating-guard-pages).
- [Microsoft acmStreamConvert](https://learn.microsoft.com/en-us/windows/win32/api/msacm/nf-msacm-acmstreamconvert)
  and [ICDecompress](https://learn.microsoft.com/en-us/windows/win32/api/vfw/nf-vfw-icdecompress):
  prepare the ACM header, check the consumed/produced byte counts, and compare
  ADPCM and decoded video with independent authored sample arrays.
- [Microsoft waveOutWrite](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutwrite):
  completion requires WHDR_DONE; submission alone is not completion.

The repository already has parent-validated desktop reports, a C: write gate,
and deferred dialogs startup after a registry round trip. Reuse these for one
fixture-name gate. Results are copied from the identified Win32 child's memory;
application stdout never supplies CIUKI_TEST evidence. Unmeasured values must
not acquire passing defaults. Unavailable observations fail explicitly and
are listed in the implementation report without changing suite predicates.

Registry recovery uses two distinct flushed values in alternating USER.CRG
slots, corrupts the newest slot's signature, then requests a real w32srv crash
through a bounded result-page request/acknowledgement. The observer identifies
the desktop's one live native `w32srv` child and acknowledges only its exit;
the production desktop supervises the replacement. The fixture reconnects and
checks the older value, then flushes a third value and verifies a second clean
service restart. Reopening a handle in the same server is never counted.

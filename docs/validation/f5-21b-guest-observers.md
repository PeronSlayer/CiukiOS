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
the production desktop supervises the replacement. Gate mode reaps only its
own exited `w32srv` child with a PID-specific nonblocking wait and calls the
production supervisor's exit handler; it leaves the PE's exit status for the
kernel observer. The fixture reconnects and checks the older value, then
flushes a third value and verifies a second forced service restart. This is
not a graceful-shutdown test. Reopening a handle in the same server is never
counted. Both acknowledged restarts are required before the result can pass.

[The upstream glibc waitpid declaration and contract](https://github.com/bminor/glibc/blob/master/posix/sys/wait.h)
distinguish waiting for one PID from waiting for any child. The repository's
`proc_wait_select`/`proc_waitpid` follow that distinction. Using `waitpid(-1)`
in the gate would consume the fixture zombie before the kernel could verify
its exit. Teardown instead snapshots the full descendant tree, stops the
desktop before its children, then lets the production collector adopt and
reap descendants. No supervisor can create a replacement during that drain.

## Observations still unavailable

- `w32-fullscreen`: the PE measures the two logical indexed dimensions and
  formats. Its public DirectDraw lease omits presenter scale. There is no
  kernel probe snapshot for scale, writes after revocation, stale-token
  refusal, rollback, restored desktop/input or measured return latency. The
  current fixture lacks synchronized transaction checkpoints for those
  facts. It marks all three affected rows unsupported and fails; requested
  dimensions never stand in for measured scale. Completing them needs a
  presenter/lease observation seam and coordinated desktop/PE checkpoints.
- `w32-input`: public calls cover layouts, copied data-format validation,
  buffer configuration, cooperative-level API rejection and teardown. The
  probe has no controlled raw motion source or foreground/exclusive
  transition checkpoints to establish buffer ordering/overflow/peek,
  cooperative loss/reacquisition and warp-free relative movement. Those
  three rows remain unsupported and fail. Empty queue reads never count as
  proof of raw input buffering.

The suite expectations are unchanged. The host publisher uses synthetic
results for grammar and negative predicate checks, not to claim those
missing guest observations passed.

## Verification boundary (2026-10-21)

Selector and probe-registry checks and the assembled boot-mode checks pass.
The three observer translation units compile with canonical kernel flags;
the shared controller's frame is 396 bytes (publisher 328, tree cleanup 32).
The nine PEs build reproducibly and resolve imports against the cached
builtin DLL exports. Startup/result-page tests initially stopped at LLVM
23.1.3 differing from the recorded 23.1.1 pins. After an external update of
the toolchain pins in this worktree, all four focused loader tests passed,
including the free/zero/RW/exact-address checks for all ten core fixtures.
The single `make CAP= apps` attempt stopped at the old SDK manifest's pinned
clang hash; SDK/apps qualification remains for the lead.
The gate and ten predicate dry-runs reach their assertions but LeakSanitizer
cannot complete under the sandbox's ptrace restrictions. Leak detection and
tool pins were not disabled or changed by this session. The sanitizer and
SDK/apps checks require a lead rerun;
no guest qualification or canonical image evidence is claimed. Exact commands
and results are in `build/codex/f5-21b-report.md`.

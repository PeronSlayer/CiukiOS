# Bounded kernel heap and page-backed Savage qualification

The `fx-r3dalloc` directive identifies a heap/allocator mismatch: the small
kernel heap accepts at most 2040 payload bytes, whereas the Savage qualification
service and its resource shadows need whole pages. Keep the heap classes and
VRAM admission unchanged; use the existing owned `desktop_buffer_alloc` path
with a byte-extent prefix for the probe service, host buffers and texture staging.
Charge rounded pages and prefixes, together with backend residency, to the existing
32 MiB render3d ceiling across both hardware scratch and scalar reference services.
Successful cleanup, partial failures, optional full-size diagnostics and mode
revalidation pair frees with the saved extent. Device-loss quarantine continues
to retain memory which a possibly active engine may reference.

Primary sources checked:

- [GCC, other builtins](https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html),
  `__builtin_constant_p`: testing an expression for constancy does not evaluate
  its side effects. A macro checks an unevaluated array bound and evaluates the
  real allocation argument once. GCC and Clang compile checks cover both `-O0`
  and `-O2`; constant requests above the shared heap geometry fail compilation.
- [GCC, common attributes](https://gcc.gnu.org/onlinedocs/gcc/Common-Attributes.html):
  the `error` function attribute diagnoses calls surviving optimization. An
  invalid constant array bound was chosen instead so the check also works
  without optimization, without adding a runtime failure symbol or panic.
- Windows 98 Resource Kit, Chapter 28, “Virtual Machine Manager / Memory Paging”,
  local read-only `build/rskt/wrkc28_4.htm` in the lead checkout: page backing is
  distinct from a small-object allocator. This fix reuses CiukiOS page allocation;
  it makes no new paging, DOS VM or hardware-qualification claim.

`ciuki/kheap_limits.h` is the single definition of the class geometry and header
charge. The real allocator checks the payload before adding its header, also
refusing dynamic requests that would wrap `size_t`. Host heap substitutes use
the same payload bound. Oversized constant callers exposed by the guard use
owned page buffers: MIDI state, COM queues, LPT firmware inventory, spool and
clock VFS tables, and NDIS admission reader/manifest. Registry parsing uses the
same small/page split in host and kernel builds. Private user-address metadata
blocks retain 64 slots on i686 and reduce slot count when wider host pointers
would otherwise exceed the same heap limit. Process descriptor vectors use one
owned page on both targets, with a page-size assertion and paired release. On
i686 this replaces a 1024-byte request occupying the 2048-byte heap class with
a 4096-byte page (2 KiB more per allocated table, at most 128 KiB over 64
process slots, with pages returned at teardown). No global memory limit changes.

Host verification commands, outcomes, the nonconstant-call audit and remaining
integration evidence are recorded in `build/codex/fx-r3dalloc-report.md`.

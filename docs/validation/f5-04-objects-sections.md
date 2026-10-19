# f5-04 objects, sections and service endpoints (2026-10-19)

## Research before implementation

Primary Microsoft API references inspected before implementation:

- [WaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjectsex): lowest-index wait-any; wait-all must not consume partial sets; abandonment and alertable timeout results.
- [DuplicateHandle](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle): shared object identity, pseudo-handle conversion, inheritance/access metadata, CLOSE_SOURCE even on failure.
- [QueueUserAPC](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-queueuserapc): per-thread FIFO, alertable delivery, callbacks execute in user mode, termination discards pending APCs.
- [SetThreadContext](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadcontext): suspended-thread context selection and privileged-state sanitization.
- [SetWaitableTimer](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer): signed 100 ns absolute/relative deadlines, periods and completion APCs.
- [CreateFileMapping](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingw), [MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile), [FlushViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-flushviewoffile): shared section lifetime independent of handles, zero pagefile backing, allocation-granularity offsets, COW and explicit writeback. FlushView does not promise a device durability barrier.
- [PostMessage](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-postmessagew), [SendMessage](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagew), [MsgWaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex): 10,000 posted-record quota, fixed records, synchronous sent-message replies and queue-class masks.
- Microsoft Ruediger Asche, [What's New in Windows 95 for VxD Writers](https://techshelps.github.io/MSDN/TECHART/html/msdn_chicvxd.htm), shipping 95 DDK article: System VM Win32 threads and VMM/VxD services. This is archival Microsoft material, not a retrieved complete Windows 95 DDK. NT handle implementation details are not asserted as 9x internals.

Repository comparison: f5-03 has VM reservations, contexts and per-thread cleanup but no Win32 handle/object manager. `uaddr.c` owns PTE and physical accounting; shared extents already retain their backing. `channel.c` already has delivery hints and scheduler donation through f0-45. `file_io_locked` is the native VFS/cache boundary. f1-82 retains synchronous FAT barriers and cache-owned dirty budgets; section flush must use VFS writes, never a second disk cache or direct device writes. The native syscall numbers/layouts remain fixed.

Decision: additive object/service records under call 96, lazy process/thread sidecars and sparse handle chunks; retain objects over waits and callbacks; reuse existing PTE ownership, ledger categories, channel objects, priorities and input hints. Named synchronization objects use the contract's flat case-sensitive namespace (including its explicit prefix stripping and ordinary backslashes). File views obey the contract's single-writer-process restriction, distinct from Windows's broader coherence guarantee. Budget evidence is host-only; QEMU and hardware measurements remain with the lead.

## Implementation and coherence boundary

The additive transport is documented in execution-abi.md. Services use native
channel descriptors, with kernel-owned role delegation and noninheritable ends;
Win32 clients obtain their initial endpoints in process parameters version 2.
Loss of w32srv removes connections and returns 1062 on reconnect, without
terminating clients. Priority translation calls `proc_priority_win32` and
`task_set_base`; focus and input use `task_focus`/`task_input_delivered`.
Foreground promotion is applied once by the existing scheduler.

Image sharing uses file SHA-256 plus fixed base, including same-base application
images. A private COW page has a physical credit before its mapping is exposed;
bytes are copied only on first write. The pagefile section's shared pages are
zero-filled on demand. Shared aliases retain their section independently of
handles, and the shared clock/gate page uses a single physical page. File
sections coalesce by VFS node identity, retaining individual section limits and
names. Writable shared aliases are restricted to one process; FILE_MAP_COPY is
private. Flush/unmap/exit writes through native VFS/cache operations. Public
flush/unmap errors retain the view for retry; exit records the writeback error.
Native VFS reads first synchronize mapped data; native writes update resident
aliases. Legacy direct FAT writers are outside this hook; broader common-file
identity and range locking remain f1-84. FlushView is not a new device barrier.

The 2 KiB production heap ceiling is explicitly reproduced by the Win32
fixture. Larger vectors and relay payloads use the existing PMM contiguous-run
allocator, with ledger RAM-commit promises and exact rollback. Private queue
and relay quotas include heap-class headers/rounding or whole PMM pages. Names
allocate only their actual UTF-16 length. Very long names, deep queues and full
handle tables still require reporting their actual allocation rather than
assuming the 512-byte per-object target covers every workload.

## Host evidence

The production harness checks wait-all all-or-none consumption, lowest-index
wait-any, pulse enrollment, timeout progression, mutex recursion and abandonment,
semaphore overflow, stale handles, access/inheritance flags, cross-process
DuplicateHandle, UTF-16 flat names/type collisions, suspended contexts and FXSAVE,
FIFO APC frames and user return, the 127-suspend limit, the 10,000-post quota,
6-byte and 64 KiB copied sent messages/replies, service authority/endpoint
handoff/loss/restart/native-call isolation, relative/absolute/periodic timers,
shared image and pagefile identity, COW, view protections/alignment, coherent
file views/native I/O, failed flush/unmap retry, separate file-section aliases,
COW allocation-failure rollback, 4096 handles and 8192 objects. One hundred
create/exit cycles include pending APCs/messages, owned mutexes, pagefile views
and image COW credits; every object/reference/payload/PMM/mapping/commit-promise
counter returns to the fixture baseline after each cycle.

These are host allocator and deterministic scheduler models, not hardware RAM
measurements. The full host kernel script also runs the independent production
ledger and process suites. Final commands, results, object table, target object
sizes and code-size totals are in `build/codex/f5-04-report.md` and its adjacent
logs/budget JSON. QEMU, image builds and hardware were not run in this worktree;
the new `w32-objects-qemu-t23` case remains unrun.

Additional lifetime checks cover demand-fault VFS waits versus unmap/protect,
early reservation of the single-writer lease, uninitialized-section refusal,
and protection upgrades of read-only image pages. Shared+COW software PTE tags
preserve read-only hardware permissions throughout protection transactions;
private COW credits are transferred once. Nested alertable APC waits retain a
stack of original contexts and resume the interrupted callback before resuming
the outer wait. None of these paths adds a panic or an NDEBUG behavior fork.

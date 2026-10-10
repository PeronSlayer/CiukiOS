# f2-03 implementation and host validation

Research (2026-10-10, before implementation):

- [POSIX open](https://pubs.opengroup.org/onlinepubs/9799919799/functions/open.html): shared open descriptions, lowest free descriptor and append placement. Keep access/status on descriptions and CLOEXEC on slots; serialize append with F1's namespace/node lock.
- [POSIX rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html): identical-entry no-op, replacement and open-unlinked lifetime. Apply the contract's case-only exception and FAT durability departure, preserving source removal before destination publication.
- [POSIX read](https://pubs.opengroup.org/onlinepubs/9799919799/functions/read.html): distinguish interruption before progress from completed bytes. Apply the ABI's issued-read drain rule to byte and directory reads, preserving output and offsets/cookies when no data reached userspace.
- [FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf): 32-bit file lengths and directory-entry timestamps. Preserve F1's COW and ordered barriers; interpret packed calendar fields as UTC per the CiukiOS contract.
- [Linux MC146818 reference](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/rtc/rtc-mc146818-lib.c): bounded UIP/seconds validation. Consume the existing provider; add no CMOS access.

Repository comparison: F1 `path_normalize` erases missing/.. lexically;
`vfs_open` excludes directories and uses 64-handle tables; `fat_remove`
unconditionally frees the chain and `fat_write` always updates an owning entry.
F2 therefore resolves components through FAT lookup under the shared VFS lock,
uses the existing process descriptor table and needs detached-entry support.
`waitword.c` already supplies MONOTONIC/REALTIME and a seed hook; `cpu_ticks`
is allocated but not charged, and `fs_timestamp` hides provider failure with a
1980 fallback. These are integration requirements, not RTC qualification.

Validation results and final interfaces are recorded below after testing.

Status: scoped implementation prepared; **f2-03 is not complete and the F2
gate is not claimed**. The directive's production allowlist excludes the
integration sites needed by the current kernel. They have not been edited.
An approval question remains pending; an unapplied, reviewable proposal is
`build/f2-03/lead-integration.patch`. In the strict tree, replacement and
open-unlink that require the missing FAT backend refuse before mutation;
the probe reports `not_run`/FAIL when its payload/backend is absent.

Files changed or added:

| File | Responsibility |
| --- | --- |
| `src/kernel/proc/fdtable.c` | Common process table, lowest free slot, shared descriptions, dup/dup2/fcntl/close, regular I/O, stdio/bootstrap and immutable executable page snapshots. |
| `src/kernel/proc/posixpath.c` | Component lookup, synthetic roots, cwd references/reconstruction, identities, stat/dirents, namespace mutations and F1 observers. Removed nodes are reclaimed after the last reference, including retained parent chains. |
| `src/kernel/proc/syscalls_file.c` | Rows 29–51; validation, copied paths, retained descriptions/cwd, user pins and completion/cleanup. Exposes the pre-issuance read-cancellation hook for F1 integration. |
| `src/kernel/proc/devnodes.c` | Null/console access rules; bounded output with raw console/capture bytes and a serial/boot-log envelope that prevents controller-record injection. |
| `src/kernel/proc/clock.c` | RTC/build seed selection through the existing f2-02 seed hook, UTC conversion, CPU accounting hook, 1 ms resolution and checked nanosleep arithmetic. |
| `src/kernel/fs/vfs.c` | Six numbered integration notes at the top: native pins at detach, shared share checks, metadata observers, final legacy orphan close, native pins in legacy mutation, legacy cwd pins in native mutation. |
| `src/kernel/probes/f2_probes_files.c` | Registered `fd-table` controller and interim file/clock cases. The controller requires a second mounted volume for EXDEV evidence. |
| `src/kernel/include/ciuki/files.h` | Native fd/path/node interfaces and lock/lifetime rules. |
| `src/kernel/include/ciuki/clock.h` | Clock interfaces and seed evidence. |
| `src/kernel/include/ciuki/vfs_hooks.h` | Weak F1 observers, allowing the original F1 harness to link independently. |
| `src/kernel/include/ciuki/fat_native.h` | Explicit missing FAT integration boundary; no unsafe emulation of detached entries. |
| `src/kernel/include/ciuki/files_probe.h` | Private interim payload record with layout assertions. |
| `tests/host/proc/files_test.c` | Production modules on the existing fake RAM and F1 fake-block harness, ASan/UBSan, filesystem and clock cases. |
| `tests/host/proc/files_payload.asm` | Interim ELF: actual user syscalls, two-thread append records, 10,000 clock reads, CPU busy/sleep, 20 ms sleep and SIGUSR1 interruption. Assembled/loaded on the host, never executed here. |
| `scripts/test/host_kernel_tests.sh` | Builds/runs the new host suite and assembles/validates its payload. |
| `tests/host/proc/FILES-REPORT.md` | Research, decisions, interfaces, evidence and outstanding work. |

Principal interfaces (wire constants and records remain exclusively in `abi.h`):

```c
bool file_syscall(struct trap_frame *tf);
bool file_read_cancelled(void);
int files_attach(struct vfs *vfs, struct px_namespace **out);
int files_bootstrap(struct vfs *vfs);
int files_stdio(struct process *p);
int file_open(struct process *p, const char *path, uint32_t flags, uint32_t mode);
int file_close(struct process *p, int32_t fd);
int file_dup(struct process *p, int32_t fd, int32_t target,
             bool exact, uint32_t flags);
int file_fcntl(struct process *p, int32_t fd, uint32_t cmd, uint32_t arg);
uint32_t file_description_count(void);
int px_resolve_locked(struct px_namespace *space, struct px_node *cwd,
                      const char *path, bool missing_leaf, struct px_path *out);
int px_getcwd_locked(struct px_node *node, char out[CIUKI_PATH_MAX]);
int px_chdir(struct process *p, const char *path);
int px_stat_locked(struct px_node *node, struct ciuki_stat *out);
int px_getdents_locked(struct file_description *d, struct ciuki_dirent *out);
int px_mkdir_locked(struct px_namespace *s, struct px_node *cwd,
                    const char *path, uint32_t mode);
int px_remove_locked(struct px_namespace *s, struct px_node *cwd,
                     const char *path, bool directory);
int px_rename_locked(struct px_namespace *s, struct px_node *cwd,
                     const char *from, const char *to);
int file_io_locked(struct file_description *d, void *buffer, size_t bytes,
                   bool write, bool positioned, uint64_t offset, size_t *done);
int file_seek_locked(struct file_description *d, int64_t offset,
                     uint32_t whence, int64_t *out);
int file_truncate_locked(struct file_description *d, uint64_t length);
int file_sync_locked(struct file_description *d);
void file_clock_start(int64_t build_epoch, bool rtc_qualified);
void file_clock_init(int64_t build_epoch, bool qualified, bool valid,
                     uint16_t date, uint16_t time, uint8_t tenths, uint64_t tick);
void file_clock_tick(void);
int file_clock_now(uint32_t clock, const struct process *p,
                   struct ciuki_timespec *out);
int file_clock_res(uint32_t clock, struct ciuki_timespec *out);
int file_sleep_deadline(const struct ciuki_timespec *request,
                        uint64_t now, uint64_t *deadline);
int file_nanosleep(uint32_t request, uint32_t remaining);
void file_clock_snapshot(struct clock_seed *out);
int probe_f2_fd_table(void);
```

All `_locked` functions require `space->vfs->lock`; that shared F1 namespace
lock also serializes file metadata, positions and append placement. `file_syscall`
returns false outside its rows and otherwise writes the signed result to EAX.
`files_bootstrap` registers the existing loader and supervisor I/O callbacks.
The boot clock caller supplies the canonical build epoch and the qualified
provider status; `file_clock_tick` must run once per scheduled PIT tick.

Required integration, currently outside the allowlist:

| Site | Missing behavior and prepared proposal |
| --- | --- |
| `fs/fat.c` | Detached regular entries must skip owning-entry writes and release only their chain at final close. Replacement must reserve alias/destination capacity before removing either name. The proposal exports `bool fat_native_ready(void)` and `int fat_rename_replace(struct fat_volume *, struct fat_entry *, struct fat_entry *, uint32_t, const char *, bool)`. It retains F1's source-removal-before-publication order. It also checks `file_read_cancelled()` before another synchronous read is issued, after any outstanding command has drained. |
| `core/syscall.c` | Call `file_syscall(tf)` before the interim desktop fd interceptor. Without this, ordinary file/clock calls still fall through to ENOSYS. |
| `core/task.c` | Call `file_clock_tick()` from `sched_tick`. Existing `process.cpu_ticks` is otherwise never charged. |
| `fs/fs_port.c` | Expose provider success separately via `bool fs_calendar_sample(uint16_t *, uint16_t *, uint8_t *)`; `fs_timestamp` alone cannot distinguish an invalid RTC from its 1980 fallback. The proposal adds no port access. |
| `core/init.c` | Register native file services and initialize the clock after F1 storage/provider setup. Qualification must remain consistent with the F1 provider's approval. |
| `scripts/build_kernel.py`, `scripts/build_image.py` | Assemble/embed the interim file payload and record a build epoch bound to the actual kernel hash. The proposal uses `SOURCE_DATE_EPOCH` when supplied and writes a build-clock record after the existing audit, then verifies its hash in the image manifest. |
| `proc/surface.c`, `proc/channel.c`, `proc/grants.c` | Include ordinary file descriptions in the reverse allocation-family quota checks. New file allocation already counts desktop descriptions; unmodified desktop allocation does not count files. |

Additional contract/integration issues to close with the lead:

- The current syscall layer has no uname (56) implementation. Its future
  `realtime_source` must consume `file_clock_snapshot`; no agreement with a
  working uname or SDK `clock_getres` is claimed here.
- The in-memory FAT extension deliberately does not promise rollback after
  a durability failure. Successful rename is serialized; I/O failure may
  remove a name or leave a lost chain, with writing revoked. No cross-link
  or Unix crash-atomicity claim is made.
- Moving an ancestor can make an existing descendant's reconstructed long
  path exceed the 259-unit creation/lookup bound. The contract gives getcwd
  no ENAMETOOLONG error. The required behavior for this case needs an
  explicit decision; the tested 259/260 matrix covers ordinary resolution.
- The interim guest controller is partial and target-compiled, not run.
  Its post-commit fault controller, durable workload/runner checker and
  agreement with uname are explicitly `not_run` and force FAIL even after
  the proposed wiring is applied. Those missing acceptance cases must be
  completed before removing the explicit gate failures. The F2 gate also
  needs full fd-table and clock QEMU evidence on all three profiles and the
  unchanged earlier QEMU gates. Host fault injection is not guest
  post-issuance/fault evidence. The proposed patch is not a gate-complete
  integration.

No QEMU, full image build, systemd scope, commit or push was run. No manual
Git command was used; the explicitly requested kernel builder performs its
existing read-only build-identity queries. Artifacts are under this worktree's
`build/`. The diary/integration record remains with the lead because the
directive does not authorize editing `dev_diary/`.

Final validation (2026-10-10):

```text
$ ASAN_OPTIONS=detect_leaks=0 scripts/test/host_kernel_tests.sh
exit 0
abi layout: PASS (687 target/native contract checks; off_t=8 time_t=8)
proc final: checks=94456 PASS
signal final: checks=4344 PASS
desktop final: checks=35970 PASS
files replacement: NOT RUN (fat_rename_replace integration absent); safe refusal verified
files open-unlink: NOT RUN (FAT detached-entry integration absent); safe refusal verified
files interrupted directory read: drained record discarded, buffer/cookie unchanged, retry succeeds PASS
files final: descriptors=0 pins=0 pages=0 checks=14837 PASS

$ ASAN_OPTIONS=detect_leaks=0 scripts/test/host_fs_tests.sh
exit 0
PASS kernel compile: 9 translation units; exact scripts/build_kernel.py CFLAGS
PASS kernel objects: no compiler runtime helpers
STORAGE RESULT checks=43488 records=190 max_record=209 failures=0 ASan/UBSan=enabled
PASS crash fsck classification: cuts=2106 unknown_diagnostics=0
PASS host filesystem suite: 14 groups, 3 fsck checks, 3 independent mtools comparisons

$ python3 scripts/build_kernel.py
exit 0
[build-kernel] FPU/SIMD audit passed
[build-kernel] build/f0/VMM.ELF (1302068 bytes), build 51bed75738e7-dirty
SHA-256 a1d87a5bfc7f99f2fe1b03b1033465c530cf75b21dee396a9fe5e9c34b795a9f
```

Full logs: `build/f2-03/host-kernel.log`, `host-fs.log`, `kernel-build.log`.
ASan/UBSan were enabled; LSan was disabled as requested. The host cannot run
the native `-m32` ABI probe (status -31), so the existing ABI harness compared
target-generated layout JSON against fixed-width host records instead.
The original F1 harness was run after the final VFS changes; later changes
were confined to native file code, its tests and the probe, which that
standalone harness does not link. No standalone-F1 result covers the proposed
FAT extension.

An additional ASan/UBSan run linked **only the proposed FAT source** into the
same new file harness, with no production-tree substitution:

```text
files open-unlink/replacement: native+legacy lifetime, detached writes/truncate, slot reuse, nlink and final-chain release PASS
files interrupted directory read: drained record discarded, buffer/cookie unchanged, retry succeeds PASS
files close fault: orphan release EIO removes fd exactly once PASS
files final: descriptors=0 pins=0 pages=0 checks=14407 PASS
```

That run's log is `build/f2-03/proposed-files.log`. It is proposal evidence,
not evidence that the strict tree implements those features. The eight
proposed C integration units were target-compiled with the kernel flags, and
the two proposed Python builders parsed successfully. The full probe with
its payload macro was separately target-compiled (the canonical builder
currently compiles its missing-payload branch); see
`build/f2-03/probe-compile.log`. Neither the integration proposal nor its
guest payload was booted. Scratch source copies and objects were removed;
the reviewable patch is retained at `build/f2-03/lead-integration.patch`,
SHA-256 `544d067bce577780a6f5e49314acb3ab04dcd13267a208a5099f75f3a549d462`.

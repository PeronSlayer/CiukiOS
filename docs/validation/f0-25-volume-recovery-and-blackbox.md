# F0-25 volume recovery and raw crash records

Research checked 2026-10-15 before implementation:

- [FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf), reserved region, FAT entries and directory size: the BPB reserves sectors before the FAT; logical file size bounds reads, and allocation beyond EOF is not another file owner. FAT[1] clean/error bits are advisory state, not chain links.
- [dosfstools ownership checking](https://github.com/dosfstools/dosfstools/blob/master/src/check.c): establish ownership before reclaiming unreferenced allocation. Preserve read access when ownership conflicts prohibit repairs. Never free allocation after an incomplete scan.
- [FatFs critical sections](https://elm-chan.org/fsw/ff/doc/appnote.html#critical): interrupted allocation and directory updates can leave lost clusters. Recovery must handle that outcome, including interruption of recovery itself.
- [ATA command specification, vendor copy](https://www.seagate.com/support/disc/manuals/ata/d1153r17.pdf): an uncertain busy/data phase cannot be replayed or reset by a diagnostic writer. A stopped channel remains quarantined; raw panic output requires a separate bounded idle-channel PIO path.

Decision: keep initial mounts strictly read-only. A completed ownership scan permits a later writable upgrade to trim the three known log chains to published EOF and reclaim unowned allocation. Reconcile differences confined to unowned clusters and advisory flags; disagreement on live links or cross-links retains write refusal. Reads do not consult the write gate. Repair writes are ordered, restartable and use the existing workspace; no memory limit changes.

Reserve 160 FAT32 sectors in new system partitions. The loader validates this geometry and uses it for both FAT and data LBAs. Sector 8 contains a versioned, checksummed descriptor; sectors 32 through 159 contain a 64 KiB raw ring. Each sector is independently sequenced and checksummed. An interrupted sector cannot invalidate the other records. Only an exact descriptor with nonoverlapping BPB metadata enables writes. Older volumes without it receive no raw writes. Replay precedes writable mount activation and is prepended to the next BOOT.LOG with `[blackbox]` tags and sent to serial. Filesystem and cache failures cannot disable this sink. A failed or busy physical controller can still prevent durable evidence; the recorder must not retry uncertain commands.

The provided 5e018550 disk log reports a writable mount and available logger, and contains no filesystem refusal record. The earlier serial captures contain interleaved fragments. These captures do not prove the specific live failure. Host fixtures qualify the independently reproducible allocation and rotation failure modes; the lead must qualify the integrated image on QEMU and the T23.

## Write and read audit

| Path | Resulting policy |
| --- | --- |
| `fat.c` sector/FAT/chain bounds, invalid links, cycles, ownership conflicts, short chains, invalid directory records | Refuse writes when live metadata is unsafe. File reads still call the read transport without consulting `readonly`; the affected malformed chain may itself return an error. Walks stay bounded by the cluster count. |
| `fat.c` ownership scan and FAT-copy comparison | Follow excess allocation without equating it to another owner. Compare decoded live entries, excluding padding, reserved upper bits and advisory flags. A completed scan permits orphan reclamation. A bad-media marker in either FAT remains reserved. An interrupted log EOF detach is recognized separately from a live-link disagreement. |
| `fat.c` reserved writer, publication and rotation | Physical writes stay within reservation capacity. Publish logical EOF before detaching a tail; reclaim only after sole ownership is established. Repeated repair is restartable. Zero-length reserved logs are supported. |
| `fat.c` write errors and durability gate | An issued I/O failure remains a write failure. Dirty flags and lost allocation are repair inputs, not hardware failures. First refusal context remains available to the UI and raw recorder. |
| `vfs.c` read/open/seek/find and log activation | Read handles do not depend on the write gate. Remaining EIO cases concern stale generations or missing/unavailable read-only backends. Rotation completes before reservation can truncate the current name. Imported uppercase 8.3 history names rename at the same owning entry without requiring an LFN slot. |
| `cache.c` admission and writeback | Failed dirty victims stay pinned. Try healthy slots for the current read; a fully pinned cache permits direct read misses. A successful run read does not fail because cache insertion fails. An issued read error is never retried by this fallback. Media generation and physical quarantine still invalidate reads. |
| `mount.c` initial scan, writable upgrade, removable media, writeback and shutdown | Initial scans issue no FAT writes. Recoverable flags/lost allocation permit explicit upgrade on fixed and removable media. Genuine delayed device failures retain the write gate. The raw worker continues after filesystem logging stops, and shutdown records request/completion separately. |
| ATA quarantine | Failed/uncertain issued commands keep the existing channel quarantine; diagnostics never reset or replay them. Emergency raw output requires IF clear, no mutex owner, no active request or bus master, and finite PIO polls. An absent currently selected slave does not prevent selecting the identified idle master. |
| DOS disk service | A readable games base opens without a COW file when creation is refused with EROFS. Sector reads continue; guest writes report write protection. Genuine I/O failures remain errors. |

The captured log does not include directory/FAT sectors. The rotation fixture therefore reconstructs its visible state with synthetic 570-line, 56402-byte contents, a dirty volume, an unpublished reserved tail and no history files. It also exercises an imported short-name owner beside an occupied slot, which independently reproduces the LFN expansion failure. It does not claim to prove that directory layout was present on the physical disk.

The raw recorder adds a 64 KiB producer queue and a 64 KiB replay buffer, plus small fields and bounded stack buffers. The supplied physical boot reports 523260 KiB usable RAM, 2254 free DMA pages and 126816 free normal pages. No memory limit or shared cache budget changes. The raw worker writes at most eight records per normal turn; rare refusal/shutdown checkpoints have a larger finite budget. A panic uses only the emergency transport and refuses a channel interrupted while owned. A quarantined or physically stalled disk cannot guarantee durable panic output.

Final host results and exact commands/output are in `build/codex/f0-25-report.md`. QEMU and T23 qualification belong to the lead: `f0-smoke`, `f3-vm`, retained earlier gates, then repeated forced power-offs on one rebuilt image. Verify BOOT1 retention, `[blackbox]` head/serial replay, settings persistence, game reads, and raw-recorder latency under the normal hardware log volume. A new canonical image is required to allocate the raw region; existing 32-reserved-sector images are deliberately never modified outside their filesystem.

Prepared removable mounts recreate their existing private FAT read windows during repair and release them before publication. This prevents a second ownership scan from degenerating into one physical request per FAT entry and exceeding the USB bind deadline. No timeout or memory budget is increased.

Shutdown checkpoints wait for an overlapping raw worker with interrupts enabled and a two-second bound. An absent recorder on an older image imposes no checkpoint requirement. A bound recorder failure is returned to shutdown; request/completion markers are not reported durable merely because another worker owned the sink. The emergency panic path stays nonblocking and separate from this wait. Raw panic writes bypass the ordinary filesystem/ATA request counters.

## Integration with f0-24 and f0-26 (2026-10-15)

Rechecked the upstream [FatFs critical-section guidance](https://elm-chan.org/fsw/ff/doc/appnote.html#critical)
and [dosfstools ownership checker](https://github.com/dosfstools/dosfstools/blob/master/src/check.c)
against the merged FAT implementation: retain f0-25's completed ownership scan,
restartable writable upgrade and exact-short-name rename alongside f0-26's
file-backed recovery check and pre-issuance cancellation hooks. No recovery
policy or memory limit changes are needed for this integration.

Retain f0-24's quota wait before sequence assignment and its bounded atomic
serial publication. Capture the formatted evidence/diagnostic record in the
RAM black box after restoring the producer's IF state, so the additional
enqueue does not lengthen that serial critical section. Each output buffer is
256 bytes; blackbox_capture takes at most 256 bytes per RAM-only lock, with
no device I/O, waits or CRC calculation. IRQ callers retain their original IF
state; panic retains its separate emergency path. UART whole-record drops do
not suppress black-box capture. Host checks establish these control-flow and
size bounds; physical interrupt latency still needs the lead's validation.

The hardware page's weak capture reference resolves to blackbox.c in the
kernel build. Page lines use klog's capture once; its direct serial-test
records keep their explicit capture. The page reports the hook as linked;
raw durability still requires a valid on-disk descriptor. Both the black-box
replay-prefix and runtime-summary bootlog regression groups are retained.
Integration commands and verbatim results are in
`build/codex/f0-25-merge-report.md`.

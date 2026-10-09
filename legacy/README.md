# Legacy archive

Historical material that is no longer part of the active project tree.

| File | Contents |
| --- | --- |
| `CiukiOS-docs-legacy-2026-10-09.zip` | The complete former `docs/` tree as of 9 October 2026: design notes, validation records, history, roadmaps, the old engineering logbook (`diario-bordo-v2.md`), the Italian devlog, the published handoff notes and the old `setup/` installer notes. |
| `CiukiOS-scripts-legacy-2026-10-09.zip` | About 370 former scripts: QEMU gates, host tests and fixtures, probes, one-off diagnosis tools, floppy and macOS scripts, plus the old `Makefile`. Reference only; the current test structure is in `docs/design/test-architecture.md`. |
| `local/` (not published) | Private local evidence: physical-PC log captures, old handoffs and scratch notes, videos/photos, and compressed backups of the original disk prefixes taken before CiukiOS was written to them. |

Extract a single file when an old decision or measurement is needed, for example:

```bash
unzip -p legacy/CiukiOS-docs-legacy-2026-10-09.zip docs/design/modular-kernel-memory.md
```

The current project record is [`dev_diary/`](../dev_diary/).

# Setup application host validation

Research checked 2026-10-23 before completing the f9-03 WIP:

- [Microsoft INF syntax](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/general-syntax-rules-for-inf-files): sections and keys are case insensitive; semicolons start comments outside quotes. `apps/setup/inf.c` follows these lexical rules. CiukiOS schema 1 deliberately rejects duplicate sections/keys, unknown keys, continuation and substitution; its required answers are those in `cd-setup-plan.md` §8, not the Windows driver schema.
- [RFC 1123 §2.1](https://www.rfc-editor.org/rfc/rfc1123.html#page-13): host labels permit letters, digits and hyphens. `setup_valid_computer` additionally enforces the plan's 15 character LAN limit, rejects edge hyphens and all-digit names. Static IPv4 checks reject network/broadcast, multicast, loopback and an off-subnet gateway.
- The repository's `sdk/libdisk/include/libdisk.h` exposes read-only `disk_enumerate`; only the real transaction may invoke its mutating phases. The application uses that boundary, and the scripted host backend counts preparation/execution and reads versus USB writes.
- The existing desktop uses one console `write` per serial record (`sdk/netsurf/port/logline.h`) and the surface/display/input grant APIs. Setup uses the same APIs; no new syscall or public header is needed.

Decisions: retain the frozen `setup_txn.h` and `setup_inf.h`. F10 follows directive item 1 (page primary action); previous WIP text saying F10 Rescue must be corrected and reviewed with the missing private artboards. The exact approved portrait is deterministically converted with the existing desktop converter. Real transaction, image, QEMU and hardware evidence belong to f9-04/f9-06.

Exact commands, outcomes and frame hashes are recorded in `build/codex/f9-03-report.md` by the implementer; render expectations live in `tests/host/setup/goldens.json`. These are host evidence only.

Host outcome (2026-10-23): six functional groups and 28 frame goldens pass under UBSan. The default ASan/UBSan run completes all functional groups, then fails at LeakSanitizer shutdown because this sandbox cannot perform its ptrace check; leak detection remains enabled. No guest or resident-memory qualification is claimed. Native linkage awaits f9-04's SDK archive; f9-01 must install Setup's display/input grants and power authority. The frozen 16-erased-record limit is enforced before prepare/execute; expanding it needs a lead-reviewed interface amendment. The UI displays required_bytes as payload with separate overhead/reserve; f9-04 must map libdisk's inclusive total accordingly.

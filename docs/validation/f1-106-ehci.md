# f1-105/f1-106 — USB inventory and EHCI host evidence

Date: 2026-10-23. Worktree: `wt/f1-106`. This is host/source evidence;
no boot image or physical controller is qualified by these results.

The binding interfaces, source links and routing policy are in
[peripheral-driver-coverage.md](../design/peripheral-driver-coverage.md#f1-105f1-106-implementation-interface-2026-10-22).
The operational extension is in [usb-contract.md](../design/usb-contract.md).
The implementer report and command logs are local under `build/codex/`.

## Coverage

`nice -n 19 bash tests/host/usb/ehci_run.sh` compiles the production engine,
core, hub, HID and BOT implementations with ASan/UBSan. Its independent
register/RAM walker checks W1C behavior, PCI command widths, ownership before
MMIO writes, pinned DMA bounds and the extended 64-bit descriptor fields.
It executes QHs/qTDs from the actual async/periodic schedule pointers.

The controller harness covers BIOS semaphore success/refusal, stuck OS
semaphore/SMI enables and cyclic capabilities; missing/unready companions;
NVIDIA/AMD/VIA quirks; low/full-speed root hand-back and disconnect;
runtime companion readiness loss and hand-back refusal;
enumeration/control and keyboard/mouse input in polling, IRQ and lost-IRQ
modes; 512-byte bulk IN/OUT, short packets, STALL, transaction errors, NAK
timeout, a late ACK during unlink, missing IAA and uncertain ASS;
hardware bInterval 1–14, periodic cancellation and two-frame reuse;
stalled clocks and corrupt links/residuals; mixed-speed hubs with explicit
unsupported TT children; mask/halt/BME/HSE stop failures; IRQ lease loss;
and HID progress while bulk waits on NAK.

The storage harness executes the production BOT, block queue, cache, FAT and
VFS code through the EHCI walker. It checks normal/short INQUIRY binding,
512-byte bulk MPS, FAT mounting, block read/write/flush and DMA stop against
private mappings of the standard host FAT fixture. It does not copy a boot
image or alter the fixture on disk.

Both new harnesses reach their PASS records, without ASan/UBSan diagnostics.
The script exits **1**, because LeakSanitizer cannot use ptrace in the
implementer's sandbox. Leak detection remains enabled. Consequently this
command is **not a green sanitizer gate**; rerun it outside that restriction
before integration. Existing USB/HID and storage runners retain their own
pre-existing sanitizer settings.

Device Manager host tests cover route-only EHCI as ABSENT with a reason,
companion parent BDF text and unchanged v1 marshalling. Existing UHCI/OHCI,
storage/recovery, worker/latency and runner tests remain applicable. Exact
commands, exit statuses and local logs are in `build/codex/f1-106-report.md`.

## Memory and limits

Layout measured by compiling baseline `HEAD` and modified headers with
Clang's `i686-unknown-elf` target, without linking a kernel/image:

| Native i686 object | Baseline bytes | Current bytes | Delta |
| --- | ---: | ---: | ---: |
| `usb_storage` | 1260 | 1264 | 4 |
| `usb_device` | 3332 | 3336 | 4 |
| `usb_host` (includes 16 devices) | 54388 | 54540 | 152 |
| Four statically retained hosts | 217552 | 218160 | 608 |
| Per-controller DMA arena | 20480 | 20480 | 0 |

The kernel-only inventory snapshot is 72 bytes. Controller count remains
4, devices/host 16, interfaces/device 4, root/hub ports 8, registry resources
64 and PCI inventory entries 32. A fully active USB host uses at most three
existing resource slots (BAR, optional IRQ, arena); four hosts use at most
12 of the 64 slots before other subsystems. Actual whole-machine occupancy
and available RAM remain hardware measurements for the lead.

An EHCI controller now allocates its own five-page arena (previously
route-only EHCI had no DMA allocation): +20 KiB per such controller, within
the unchanged aggregate maximum of four 20 KiB host arenas. Allocation
failure keeps EHCI route-only so companions can still serve USB 1.x ports.
No physical memory limit or registry capacity is increased.

## Remaining qualification

Lead cross-review of the inventory interface/routing policy, a green
LeakSanitizer run outside the sandbox restriction, the canonical build and
physical SMM/port routing/PCI IRQ/DMA behavior remain unqualified here.
QEMU, systemd scopes and image builds were outside this implementer's task.
TT splits remain explicitly unsupported. Boot HID has a 64-byte endpoint
bound, one transaction per microframe and a 1024-frame maximum interval;
BOT uses the common per-packet engine with an eight-sector command bound.
No high-speed throughput claim is made from these host models.

# 0.8.3 bounded release validation

The Linux QEMU/KVM full-HDD integration run passed. The command was
`python3 scripts/qemu_test_native_gpu.py --release-ui --output <new-directory>`,
inside a systemd scope with MemoryMax=768M, MemorySwapMax=0, CPUQuota=200%,
TasksMax=128 and a 310-second outer timeout. One VM ran at a time, after builds
had completed. No continuous trace, recording or full RAM dump was used.

[Raw results](results.json) cover a Doom new game, rotation, normal exit and GPU
release, NE2K PCI NAT DNS/HTTP and reload, PS/2 wheel down/up pixel checks, 32-bit
mode timeout/rollback and acceptance, 16-bit banked preview/rollback, saved mode
and About preference across reset, and reopening About at 640×480. GUI operations
produced no full-screen DOS launch markers. F4 at the end intentionally entered DOS.
Input was injected through QEMU; this does not certify a physical mouse or keyboard.

[Final cadence](final-cadence.json) uses a fixed 32-KiB guest timing ring and the
last 9.9 seconds of rotation. Doom averaged 34.78 game-loop starts/s. GPU submission
spacing had a 30.87-ms 95th percentile, 67.73-ms maximum and two gaps over 42 ms.
Capture itself had a 0.108-ms median and 0.546-ms maximum. The host scope recorded
zero CPU quota throttling. These are software events, not physical monitor FPS.
Residual spikes remain; this is not proof of zero visual stutter.

At the same 800×600 mode, the [earlier run](before-precise-desktop-damage.json)
had six submission gaps over 42 ms; the [first corrected run](after-precise-desktop-damage.json)
had one, and the final run had two. Each is a short sample, not a statistical
hardware benchmark. The older baseline used 1280×768 and is not a matched-resolution
comparison. The fixed cause was a desktop meter callback expanding a small update
into a full-scene repaint approximately every 989 ms.

The initial wheel gate used the reversed QEMU HMP Z direction; the corrected gate
checks actual image changes and signed dispatch. A separate mode-selection run
exposed a real scratch-buffer overrun, fixed at the WORD-capacity load in the
compositor. The reset fixture also needed `-action reboot=reset` to override its
shared `-no-reboot` option. Earlier failed results remain under local build/tests;
none are counted as passes. See the [VBE analysis](../2026-10-04-vbe-bank-boundary.md).

Host checks: 73 VirtIO protocol checks and 1,285 peripheral model assertions with
ASan/UBSan passed. The canonical full build produces the sanitized Windows ZIP;
its CRC, manifest and payload exclusions are checked on Linux. No Windows runtime,
physical graphics-card or full-CD runtime qualification is claimed here.

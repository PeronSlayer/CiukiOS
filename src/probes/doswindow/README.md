# DOS text-window runtime acceptance probes

These are standalone real-mode DOS programs, not UI simulations. They contain
no CiukiOS-private calls, no window-manager callbacks and no host-side delays.
Build from the repository root:

```sh
bash src/probes/doswindow/build.sh
```

The generated programs and NASM listings are under `build/probes/doswindow/`.
Copy the six programs into a **writable test directory** on an isolated image.
Keep the matching child COM next to each parent and run from that directory.
`DWCHECK.DAT` must not already exist. The probes refuse to overwrite it; they
remove only their own newly created scratch file after verification.

| Parent | Rendering | Nested child | Acceptance scope |
| --- | --- | --- | --- |
| `DWBIOST.COM` | BIOS INT10 only | `DWBIOSCH.COM` | BIOS text-window COM |
| `DWBIOST.EXE` | BIOS INT10 only | `DWBIOSCH.COM` | BIOS text-window MZ |
| `DWTEXT.COM` | Direct B8000 writes plus BIOS scrolling | `DWCHILD.COM` | Future direct-memory monitor COM |
| `DWTEXT.EXE` | Direct B8000 writes plus BIOS scrolling | `DWCHILD.COM` | Future direct-memory monitor MZ |

The BIOS-only variants, including their nested child, compile without any
B8000 read/write instruction. They use INT10 AH=02/09 for cells and AH=06/0E
for a scrolling region. All variants query AH=0F and require logical mode 3,
80 columns, page zero. They **never set a hardware video mode**. A native
window backend must provide that ordinary logical text-mode contract.

Each parent normally runs for 728 BIOS ticks, approximately 40 seconds:

- It updates an animated cell and visible hexadecimal tick/frame counters
  every two real BIOS ticks, approximately nine frames per second.
- Once per 18 ticks, it scrolls rows 12–23, columns 2–77 using BIOS INT10 and
  writes a new tick value in that region.
- At startup and again near second 20, it creates a file, writes a payload
  containing text and binary bytes, closes/reopens it, verifies the exact
  length and contents, closes it and deletes it.
- Near second 12, it uses ordinary DOS AH=4B to execute the matching child.
  The child animates for 37 BIOS ticks and exits with code 5A. The parent
  checks both DOS termination status and restoration of its original PSP.
- Ordinary keystrokes appear as BIOS scan/ASCII codes on screen and in the
  log. ESC exits normally through DOS AH=4C. Early ESC is allowed, but cannot
  satisfy the minimum-duration acceptance criterion.
- Idle time uses STI/HLT between actual timer wake-ups. It does not invoke
  DOS INT28 or another cooperative host scheduling hook.

Markers go directly to COM1 (using its existing configuration, with bounded
transmitter polling) and QEMU debug port E9. COM1 should be captured as serial
output. They have distinct prefixes such as `[DOSWIN:BIOS-COM]` and
`[DOSWIN:DIRECT-MZ]`; all numbers are hexadecimal. Typical sequence:

```text
[DOSWIN:BIOS-COM] START psp=....
[DOSWIN:BIOS-COM] FILE OK bytes verified and scratch removed
[DOSWIN:BIOS-COM] LIVE ticks=0012 frames=0009
...
[DOSWIN:BIOS-COM] EXEC BEGIN ticks=00DA frames=006D
[DOSWIN:BIOS-CHILD] START psp=....
[DOSWIN:BIOS-CHILD] END status=005A
[DOSWIN:BIOS-COM] EXEC RETURN ticks=.... frames=....
...
[DOSWIN:BIOS-COM] FILE MIDRUN OK ticks=.... frames=....
...
[DOSWIN:BIOS-COM] END ticks=02D8 frames=.... reason=TIMEOUT
```

An error emits `FAIL` and terminates with status 1. Successful timeout or ESC
terminates with status 0. BIOS timer midnight wrap is accounted for. A custom
`RUN_TICKS` or `ENABLE_NEST=0` build is possible for development, but must be
reported as a different test; it cannot silently replace the default gate.

## What constitutes a real concurrent-window pass

Capture guest serial output, actual screenshots and host input commands with
timestamps. A marker or a screenshot alone is insufficient.

1. Launch a parent in a DOS window. Record its `START` PSP and confirm the
   animated cell, tick/frame numbers and scrolling region are visible inside
   that window. Verify at least 182 elapsed ticks (about ten seconds) while
   the parent remains alive. Default completion also needs the mid-run file
   verification and nested child sequence.
2. Between two increasing `LIVE` samples from this **same uninterrupted
   parent**, drag a different, already-open native window and minimize/restore
   the DOS window through its real taskbar button. The current interrupt-time
   host deliberately does not open new dialogs or invoke DOS while a child
   owns execution. Show the changed geometry/control result in screenshots while the
   DOS counter also advances. There must be no parent `END` and no relaunch
   between these events. A UI that resumes only after the DOS process exits
   fails this criterion.
3. Focus the DOS window and type known ordinary keys; correlate the visible
   scan/ASCII result with its `KEY` markers. Activate another native window
   and verify keys intended for it are not delivered to the unfocused child.
   Refocus the DOS window and verify input delivery resumes.
4. Verify the nested child actually starts under a distinct PSP, exits 5A,
   and the original parent's counter resumes without another `START`.
   Verify `DWCHECK.DAT` is absent afterward and the file-check markers report
   success. These checks exercise real DOS services, not fabricated results.
5. Test automatic timeout and, in a separate run, ESC after at least ten
   seconds. Both must restore a responsive native desktop with no abandoned
   text pixels, stuck input ownership or resident probe process.

A normal full-screen DOS run is a useful baseline for the guest binaries. It
**does not prove concurrent GUI execution**. Passing `DWBIOST` does not prove
direct-memory `DWTEXT` works: that separate gate requires a monitor that
virtualizes the legacy text aperture without damaging the native framebuffer.
Neither gate establishes support for arbitrary DOS graphics, protected-mode
DOS extenders or Windows 3.x.

The native BIOS-text lane can be exercised with the immutable packaged image
and its matching NASM shell listing:

```sh
uv run --with numpy --with pillow python scripts/qemu_test_dos_window.py \
  --image path/to/candidate.img --listing path/to/matching-shell.lst \
  --output path/to/new-evidence-directory
```

This lane uses read-only guest memory observations, real QEMU keyboard/mouse
events and pixel comparisons against the actual BIOS font and VBE palette.
Its default gate includes both COM/timeout and MZ/ESC runs, interactive shipped
`COMMAND.COM`, cooperative title-close, and a full-screen child after removing
the window hooks. `--com-only` is a narrower development run, not the full gate.

The new probes supplement, rather than replace, real-program acceptance:
run the shipped `COMMAND.COM` in a window, execute `DIR`, perform a normal
child COM launch and return to that same interpreter. Exercise the existing
`CIUKEDIT` editor's typing/save/exit flow if the BIOS-text backend supports its
console access path. Record any direct-memory use or unsupported service
honestly instead of replacing the real program with a probe.

## Relative performance measurements

`DWPERF.COM` is a separate real-mode BIOS-text workload. It submits full 80×25
screens through 25 real INT10/AH13 calls per batch, then runs real AH06 scrolling
plus AH13 line output. Each phase lasts 182 observed BIOS timer ticks. It reports
the submitted batch count, actual BIOS call count, elapsed BIOS ticks and a
CPUID-serialized 64-bit TSC interval. A submitted batch is **not** a displayed
frame, and this workload is **not** a DOS game benchmark.

```sh
uv run --with numpy --with pillow python scripts/qemu_test_dos_window_performance.py \
  --image path/to/candidate.img --listing path/to/matching-shell.lst \
  --output path/to/new-performance-evidence --label candidate
```

The harness copies the image and adds only `APPS/DWPERF.COM` to that copy. It
observes an idle shipped COMMAND.COM window, measures real mouse input, then
runs the two active workloads. After the initial shell-location lookup, timed
measurements read only the runtime's small 128/192-byte header. The report
includes the number of reads and their accumulated monitor overhead. Mouse
latencies are upper bounds from host input submission to an observed completed
callback with the new coordinates; an actual screenshot checks the rendered
pointer after idle trials. There are no guest memory writes or paused-VM clocks.

Runtime ABI 2 exposes completed paints and cumulative/max elapsed TSC for the
callback and paint operations. Those elapsed counts include nested interrupts;
they are not exclusive CPU-cycle attribution. ABI 1 has no paint counter and
the harness does not manufacture one from callback frequency. QEMU process
CPU time is measured independently. `--compare path/to/prior/report.json`
requires the same probe bytes and QEMU CPU/memory/video configuration.

KVM uses the host CPU rather than reproducing Pentium III execution speed.
These results support relative comparisons under the same host configuration;
they cannot establish physical old-PC frame rates, hardware acceleration, or
30-FPS compatibility for games using VGA memory or protected-mode extenders.

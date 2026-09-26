# Keyboard LED interrupt repair — 2026-09-06

The physical T23 report is: root `COMMAND.COM` returns `cannot execute`,
directory names remain corrupt, and pressing Caps Lock leaves the computer
blocked without restarting. The file/EXEC failure is still unresolved on the
physical machine. The user explicitly requested stopping before CD writing;
no CD has been written during this work.

## Reproduced keyboard defect

CiukiOS's IRQ1 hook had an unconditional guard against reentry while its BIOS
chain was active. The guard sent PIC EOI and returned without allowing the
BIOS to consume a pending keyboard byte. This also discarded legitimate
nested acknowledgements during keyboard LED updates.

The [IBM PC AT Technical Reference, Keyboard pp. 5-116 and 5-121](https://bitsavers.trailing-edge.com/pdf/ibm/pc/at/1502494_PC_AT_Technical_Reference_Mar84.pdf#page=259)
documents the relevant behavior: the LED routine sends EOI, writes the
keyboard command, enables interrupts, and waits for IRQ1 to record ACK or
RESEND. By contrast, [SeaBIOS updates LEDs from INT 16h](https://github.com/coreboot/seabios/blob/master/src/kbd.c),
which explains why ordinary Caps Lock tests with that BIOS did not exercise
this reentrant path. This is a firmware behavior model, not a dump or trace
of the T23's BIOS.

`scripts/fixtures/keyboard_led_irq.asm` is a test-only option ROM. It leaves
ordinary scan-code translation to SeaBIOS, then performs LED updates inside
IRQ1 using actual PS/2 `EDh` and LED-mask writes. Actual keyboard ACK bytes
must arrive through nested IRQ1 calls. It does not manufacture ACK bytes,
application output, or DOS return values. It is absent from product images.

The shipped kernel receives zero ACKs and records two timeouts after Caps
Lock. A separate control replaces just the entry of CiukiOS's IRQ1 handler
with a direct jump to its saved BIOS vector; that control receives all 28
ACKs across 14 updates and has no timeouts. The control is not a product
change and is recorded byte-for-byte in `bios-passthrough.json`.

## Product change

The reentry guard now checks port 64h. If a keyboard byte is pending, it
chains directly to the saved BIOS handler, leaving port 60h untouched for
that handler and retaining the outer guard. An empty duplicate interrupt
still returns through the guard; auxiliary mouse data does not qualify as a
keyboard byte.

The candidate passes the same 14 LED updates and 28 actual ACKs, with zero
timeouts or unexpected responses. Subsequent DIR, nested COMMAND.COM,
COM/MZ execution and return to the parent shell work. A separate optional
control injects one empty software INT 09h while the outer hook is active;
it returns once without reentering the BIOS, and all LED updates still pass.

Candidate Windows 3.1 at 800×600 passes two sessions, Calculator, window
resize/repaint, WAV/MIDI, child close and DOS return. Wolf3D and Doom Vanille
reach gameplay, respond to movement, emit PCM and quit to a working shell.
These are emulator results; physical T23 confirmation is outstanding.

## Evidence

Directory: `build/full/keyboard-irq-repair-2026-09-06/`.

- `before-led-irq/result.json`: original kernel, blocked real ACKs.
- `control-bios/result.json`: direct BIOS-chain control.
- `after-led-irq/result.json`: corrected kernel, actual ACKs and DOS return.
- `after-led-and-empty-irq/result.json`: correction plus empty-reentry guard.
- `standard-locks/result.json`: original kernel with ordinary SeaBIOS,
  Caps/Num/Scroll Lock in desktop, graphical DOS and nested COMMAND.COM.
- `windows-800/result.json` and `games/results.json`: application checks.

The baseline build was first verified byte-identical to the shipped kernel,
SHA-256 `7c482ad57a0bb44d3456fb0a5e999d13bbe5743468731a4e739f5186f671d1e3`.
The keyboard candidate is 43211 bytes, within the 43264-byte kernel slot,
SHA-256 `e795ad75964280dfdc064a6de23c5191fbe6aa7d6f8cefcabca6d59eb9cf4cbc`.

## Reproduced shell error propagation defect

The shell's `exec_try_current` and `exec_try_prefixed` compared an EXEC error
against 2 before returning it. For larger errors such as 5, that comparison
cleared CF. The caller then treated a failed load as a successful launch.
This concealed the real error; the truncated program itself was not entered.

The disposable test image contains a one-cluster SHORT.COM whose directory
size deliberately claims two clusters. GDB records show the kernel detecting
the short chain and returning AX=0005 with CF set, all the way through
INT 21h to the shell. The following comparison in the shell clears CF. The
recordings are in `short-chain-trace-manual/` and
`short-chain-return-trace/`; `exec-codes/` retains the failed pre-fix test.
The earlier GDB attempts are setup/trace attempts, not acceptance passes.

The failure paths now explicitly set CF after their comparisons, and the
last extension attempt branches separately on success. Both the native
shell and COMMAND.COM use the repaired routine. The shell also preserves AX
across desktop video restoration and prints its numeric DOS error.

`exec-codes-fixed/` passes real missing-launcher error 0002, actual short-chain
error 0005 in native and compatibility interpreters, subsequent valid COM/MZ
execution and desktop return. No DOS result is injected. The test image's
deliberately broken files are not included in product builds. These changes
do not establish that the physical HDD directory corruption has been fixed.

## Candidate ISO, stopped before CD writing

The candidate is
`build/full/keyboard-irq-repair-2026-09-06/CiukiOS_candidate_2026-09-06.iso`,
SHA-256 `3fbb3b91457d70eb9ce56956e124bb878456f53c75f1a30aefb20c19a51d37e2`.
The preceding ISO and raw CD image are archived in the evidence directory's
`previous/` subdirectory. The optical drive was not accessed.

Comparing all 1408 payload files finds exactly four changes: CIUKIDOS.SYS,
SHELL.COM, root COMMAND.COM and Windows' copy of COMMAND.COM. All application,
audio and setup payloads remain byte-identical to the preceding CD image.
The runtime-ownership check rebuilt a separate flat image; that image was
subsequently rebuilt with the same qualified audio payload directory. This
does not alter the candidate ISO or raw CD disk.

The actual candidate ISO was installed by its graphical Setup onto a
disposable copy of the preceding HDD. Every installed sector matches the
source, allowing only the expected GRUB identity and D-to-C boot patch. The
installed HDD boots without the CD and accepts commands. Its SHA-256 is
`195d1e8b333818229206a6ec9234771e20d5614ef4d2bc62c10def9c357b3ce7`.

On copies of that final installation, the LED/empty-reentry test, real EXEC
error tests, and graphical DOS directory/complete-file checks all pass. The
directory test also injects 1090 actual mouse movement commands and finds no
unexpected changes in 38675 kernel-code bytes. Its SBEMU screenshot is
`final-paths/sbemu-directory.png`; the text-mode error screenshot is
`final-exec-codes/text-error-code.png`.

Windows at 800×600 and classic Doom were checked with both the exact final
kernel and native shell before packaging. Classic Doom responds to the menu
selection sequence, reaches gameplay, emits PCM and returns to working DOS.
The complete manifest, payload differences and test results are recorded in
`candidate-manifest.json`. This remains a candidate with two reproduced fixes;
it is not proof that the user's physical directory corruption is resolved.

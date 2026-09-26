# Installed HDD directory/EXEC regression — investigation

Status: unresolved on the physical ThinkPad T23. No product-code fix or new
release is asserted by this report, and no CD was written during this
investigation.

The latest physical report is specifically `cannot execute` / `EXEC FAIL
0002`, rather than a freeze inside a running game. The user also reports
that `DIR \SBEMU` displays corrupt names and emits a beep. Earlier photographs
show the same kind of corruption in `\WINDOWS`. This requires investigating
directory data and executable lookup before treating individual game or
Windows startup behavior as the cause. The beep's source is not established.

## Exact input and controls

Tests use disposable copies of the actual final HDD produced by Setup:
`build/full/hdd-startup-repair-2026-09-06/install-final/target.img`, SHA-256
`64f18a13b1c76e0071b8c963e21c2138549eb58dd4893b139ec987c5d2fc9410`.
The original HDD image is hashed again after execution to check that it was
not modified. The source here is an emulated installed HDD, not a capture
from the user's physical HDD.

The previous bulk filesystem tests forced `DISPLAY.CFG` to `TEXT`. They did
not cover directory enumeration through the graphical DOS console. The
installed-HDD test now accepts `--keep-display-profile` and `--vga cirrus` to
exercise the shipping profile and a second video BIOS/device. A
`--pointer-activity` option injects real relative mouse movement while the
guest enumerates directories and reads files.

A separate test-only shell control keeps the original image length/layout,
skips startup display services, and calls the existing direct-DOS entry
instead of entering the UI. It does not replace the product shell. The exact
byte edits and both hashes are recorded in `direct-dos-patch.json`.

## Results and their limits

Evidence directory: `build/full/ui-exec-isolation-2026-09-06/`.

| Case | Observation |
|---|---|
| `std-gui-paths` | Shipping AUTO profile, desktop to DOS, lowercase paths, every enumerated short name, relative file checksums, COM/MZ return: pass. |
| `cirrus-gui-paths` | Same sequence with Cirrus video BIOS/device: pass. |
| `cirrus-gui-motion` | Same sequence with 737 mouse movement commands and no monitor-thread errors: pass. |
| `cirrus-direct-dos-fixed-check` | Test-only direct text entry, directory/file/COM/MZ checks: pass; startup intentionally silent. |
| `sbemu-cirrus-gui` | Same graphical transition, all 12 SBEMU names, complete reads/checksums of both HDPMI and both VSBHDA components, remaining path tests: pass. |
| `nested-command-checked` | Actual shipped root COMMAND.COM opens in BIOS text mode 03, lists all 12 SBEMU names, executes COM/MZ children and exits to its parent and then the desktop: pass. |
| `reinstall` | Actual ISO Setup installs over a disposable copy of the previous installed HDD; all sectors agree and the resulting hash equals the final HDD above. |

The path tests compare guest output against an independent FAT16 parser, and
the file checks read all bytes through the guest DOS API. The checksum probe
is copied only into the disposable test image. Each path run also compares
38661 immutable kernel-code bytes against the source image. No unexpected
code writes were found; this check does not cover all mutable kernel data.

The first direct-DOS trial, `cirrus-direct-dos`, passed its guest checks but
failed its host audio validator: intentionally absent startup PCM was passed
as an empty slice to the WAV parser. That run is retained as a failed trial.
The validator was corrected and rerun in a separate directory. It is not
counted as evidence of a guest failure or silently relabeled as a pass.

The first nested-COMMAND trial also had a validator error: it expected the
native UI motto in the compatibility interpreter's different banner. The
interpreter had started successfully. That trial is retained separately as
`nested-command`; the corrected reproducible test is
`scripts/qemu_test_installed_command.py`. Its text-mode directory screenshot
is `nested-command-checked/sbemu-text.png`.

The new SBEMU test reads 37120, 37376, 111616 and 69270 bytes respectively
from HDPMI32I.EXE, HDPMI16I.EXE, VSBHDA.EXE and VSBHDA16.EXE, comparing the
guest's complete-file checksums with the actual installed source payloads.
The test does not demonstrate that these components can be found on the
physical HDD, where the user's output remains corrupt.

## Physical discriminator result

The installed image already contains root `\COMMAND.COM` (14560 bytes,
SHA-256 `9c38d12fcacb279311f3fadf20c1e32ff65871fb19d68b07147dbb09a9e7b0c1`).
It is the text-only `COMMAND_COMPAT` build of the command interpreter, and
with no arguments it opens an interactive nested interpreter. The requested
physical check is `run \COMMAND.COM`, followed by `DIR \SBEMU`.

This can distinguish corrupt output confined to the graphical console from
corruption still visible through the text interpreter. Failure to launch the
root interpreter is a third useful outcome. This control still uses the same
kernel and occurs after startup; even a successful text listing cannot prove
that earlier UI activity left all kernel state intact. The user subsequently
confirmed that root COMMAND.COM itself returns `cannot execute`, and that
Caps Lock leaves the PC blocked without a restart. See the separately
[reproduced keyboard LED interrupt defect](keyboard-led-irq-repair-2026-09-06.md).

## Further physical observations

The user subsequently confirmed a cold HDD boot with no CD still has the
failure, while `DIR \` lists the root correctly. The failure is therefore not
explained by the installer reset path alone. The data/subdirectory read path
remains under investigation. The [UI/setup repair report](ui-setup-repair-2026-09-06.md)
records the separately reproduced display-menu defect and the completed
emulator checks, without claiming a physical filesystem repair.

# CiukiOS hardware identification names

`pci-ids.json` and `usb-ids.json` contain only bus ID pairs and plain ASCII
device labels. They contain no vendor text blocks, INF filenames or driver
payloads. Names are UI labels only; matching an ID does not establish driver
support or authorize hardware access. The Device Manager reads the PCI file
only on its first name lookup. Both files are installed at `/System/hwids/`.

## Generation provenance

- Source: owner's read-only **Ultimate Windows Driver Pack (for
  vista,xp,98,95,me,2000,2003)** INF collection.
- INF files found: **909** (all case variants of `.inf`).
- Generated: **2026-10-13**.
- Generator: `scripts/build_hwids.py`; rerun with the collection directory as
  its argument, or set `CIUKIOS_INF_DIR`. The source collection is external,
  git-ignored and never copied into the image.
- Output: 1,155 PCI IDs and 218 USB IDs; combined JSON size 75,014 bytes.
- Each pair is deduplicated by choosing the shortest non-empty resolved
  description (then case-insensitive lexical order for stable ties). Missing
  descriptions are omitted. IDs and keys are lower-case hexadecimal.

The names are derived data from third-party Windows INF metadata. The Windows
driver binaries remain outside CiukiOS and are not executable as kernel
drivers or included in the image.

F4-11 adds three AudioPCI controller names absent from the INF-derived table
(`1274:5000`, `1274:1371`, `1274:5880`), using the Linux ens1370 device table
referenced in [the transport notes](../../docs/design/f4-audio-second-set.md).
These are supplementary names; they do not alter the INF generation counts
above or imply physical qualification.

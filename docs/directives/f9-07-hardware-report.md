# f9-07 — Hardware report for public trials

- **Phase/model:** F9, Codex gpt-6.1-sol, reasoning high. **Worktree:** `wt/f9-07`. Starts after f1-109 (ACPI/PnP inventory ABI) merges.
- **Owner (2026-10-22):** a CiukiOS distribution that anyone can try on their own retro PC. Testers need a one-click way to send back what their machine is and what worked.

## Required

1. **Hardware Report application** in the Programs → System Tools menu, also offered by Setup's rescue page.
   - **What it collects:**
     - CPU (CPUID vendor, family, model, stepping, flags) and the memory map summary;
     - SMBIOS identity (vendor, product, BIOS version and date);
     - PCI devices with names, resources and the bound driver or reason; USB devices; the ACPI/PnP inventory (f1-109);
     - display: adapter, bound 2D driver, accelerated or software engine, modes;
     - audio, network and storage bindings, including chipset ATA mode;
     - the boot log and the latest kernel records, with the image SHA-256 and build manifest.
   - **What it never collects:** user files, network addresses or host names beyond the interface type and link state.
2. **Output:** one plain-text file, `CIUKIREP.TXT`, plus an optional compressed archive of the full logs, saved to a chosen drive. A USB stick or floppy is the default when present.
3. **The user sees what is saved.** A summary page lists every section, and a privacy note explains what is and is not included.
4. **Host tests** cover the formatting of each section with fixture inventories, the exclusions, and write failure on a full or removed medium.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Write `build/codex/f9-07-report.md`.

## Amendment 1 (lead, 2026-10-23 12:19): Setup rescue wiring and build provenance

Context: f9-07 merged before the Setup applications (f9-03/f9-04b/f9-05) landed, and its report left two items open: the Setup rescue page still shows Setup's own report (`apps/setup/hw.c`, `ST_ID_REPORT`), and the Hardware Report says the build provenance is unavailable although f9-05 now installs `/System/BUILD.TXT` (`scripts/build_image.py`).

1. **Provenance.** The Hardware Report reads `/System/BUILD.TXT` (bounded, read-only, through the existing file API) and quotes it in the build section: source commit, kernel ELF SHA-256, manifest id, date. A missing or malformed file is reported as such, never invented. The image SHA-256 stays out (an image cannot contain its own hash). Host tests: present, missing, oversized, malformed.
2. **Setup rescue.** The rescue page's hardware report action starts the f9-07 application with `--rescue` and the desktop channel grant the f9-07 report requires (no bare spawn without a channel, no normal launch that permits ATA export). The application must be present in the Setup CD payload (`scripts/build_cd.py`) next to Setup; if it cannot be started (missing binary, spawn refusal), Setup falls back to its own report and logs `[rescue] action=hardware_report result=fallback reason=<r>`; on success it logs `result=app`. Setup's existing "Save hardware report to USB" keeps working.
3. **Tests.** Setup host tests for both outcomes (app started with the exact arguments and channel; fallback), the CD payload check for the binary, the existing Setup goldens updated only where the rescue page text changes.

Scope: `apps/hwreport/`, `apps/setup/` (rescue action and its host hooks), `scripts/build_cd.py` payload list, their host tests, `docs/design/hardware-report.md`. Not in scope: kernel, ABI headers (if the spawn needs a new ABI record, stop and report), other Setup pages.

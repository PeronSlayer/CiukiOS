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

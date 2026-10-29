# fx-msi: Windows Installer (MSI) through the reused Wine msi.dll

- **Written:** 2026-10-29 12:16 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-msi`, branch `wt/fx-msi` from `main`.

## Lead decision: amendment to the Win32 personality contract

`docs/design/win32-personality-contract.md:257` says "MSI MUST be refused with a clear message", and `win32-wine-dependency-audit.md` row `w21-msi` follows it. **This is superseded.**
- Windows Installer 1.x reached Windows 98 through the setup programs themselves (`InstMsiA.exe`, redistributed with Office 2000 and many 1999–2002 applications). Windows ME shipped it.
- Applications of the target era therefore install through MSI, often after bootstrapping it.

CiukiOS supports Windows Installer:
- through Wine 10.0's `msi.dll` and `msiexec.exe`, reused as further upper DLLs like the existing ones (`config/wine-upper.json`), with their LGPL source and notices carried by the release machinery;
- reported as installed, so a bootstrapper's `InstMsiA.exe` step is skipped the way the Windows Installer redistributable skips an equal or newer version. Research the registry and file checks those bootstrappers use, and record them.

## Scope

1. **Contract and audit.** Amend the contract and the dependency audit with this decision and the sources: Microsoft Learn "Windows Installer" and its version history (Windows Installer redistributables) and Wine's `dlls/msi`.
2. **Build.** Add `msi.dll` and `msiexec.exe` to the reused upper set with the existing pinned Wine archive and recipe (`scripts/win32/`), together with any dependency the audit shows missing (for example `sxs`, `wintrust`, `fusion` stubs). Prefer the existing CiukiOS implementations where they exist. Run the H1 harness's DllMain attach/detach for each new DLL and add their rows wherever the existing DLLs have them (census, `config/wine-upper.json`, policy rows).
3. **Registration and security.**
   - The `.msi` file association runs `msiexec /i`.
   - Per-machine installs require administrator rights, and the Windows Installer service work runs through the existing system-services path, never as a privileged user process.
   - Per-user installs stay in the user's profile.
   - Advertised shortcuts (`w21-msi`) resolve.
   - Add/Remove Programs lists MSI products, and uninstalls them through `msiexec /x`.
4. **Tests.**
   - Generate a small MSI on the host with Wine's tools or Python `msilib`-equivalent code you write; do not use a downloaded third-party MSI. It should install files, a shortcut and a registry value, with a custom action of type 51 (set property).
   - Run it through the H1 harness: install, its Add/Remove entry, repair and uninstall.
   - Cover a standard user refused for a per-machine install, and an `InstMsiA.exe`-style version check that skips the bootstrap.
5. Run the Win32 suite, the security suite and `python3 scripts/gen_api_reference.py --check` sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/fx-msi-report.md`.

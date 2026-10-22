# f2-100 — Complete network and network-adapter settings (Control Panel)

- **Phase/model:** F2/F6, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f2-100` from `main`.
- **Owner request (2026-10-22 22:58):** complete configuration for the network and the network card.
- Reference: the Windows 98 Network control panel and TCP/IP properties (IP address: DHCP or static with mask; gateways; DNS servers and host/domain; adapter properties: driver, resources IRQ/I/O, advanced options such as media type, duplex and speed) — cite the Resource Kit (`build/rskt` in the main checkout, read-only) — adapted to CiukiOS. Syscall 90 `netctl` is reserved (ENOSYS today).

## Required

1. **Kernel `netctl` (syscall 90):** list interfaces and adapters (driver, PCI id, MAC, IRQ, I/O, link, speed, duplex, rx/tx/error counters), get/set the IP configuration per interface (DHCP on/off, static address, mask, gateway, up to 3 DNS servers, host name, domain), renew/release DHCP, enable/disable the interface, adapter options supported by each driver (e100, e1000, the other NICs: media auto/10/100, half/full duplex) with validation and records `[net] config iface=… mode=dhcp|static …`; capability-checked (desktop/Control Panel grant only).
2. **Persistence:** the configuration is stored in the system configuration (registry or `C:\SYSTEM\NETWORK.INI`, choose the existing pattern) and applied at boot before DHCP; a broken configuration falls back to DHCP with a record.
3. **Control Panel → Connections → Network:** a classic dialog with the adapter list, adapter Properties (driver info, link status, advanced options), TCP/IP Properties (obtain automatically / specify address, gateway, DNS), a Status view (address, gateway, DNS, lease, packets sent/received, errors) and a Test button (resolve a name and connect to the gateway) with the results; the tray network icon opens it.
4. **Tests:** netctl on the host network model (set static, back to DHCP, invalid values refused, persistence, boot application), the dialog golden frames, the capability check. Touch NetSurf only through the system resolver.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Write `build/codex/<id>-report.md`.

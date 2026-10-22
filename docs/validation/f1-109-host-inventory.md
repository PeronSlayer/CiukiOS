# f1-109 host evidence

Verified 2026-10-23. Scope: synthetic firmware and host compilation only.
The binding ABI and research decisions are in
[peripheral-driver-coverage.md](../design/peripheral-driver-coverage.md#f1-109-inventory-abi-and-read-only-interpreter-boundary).

The vendored release matches all 207 audited Git blob hashes: 164 C files,
43 headers, 3,499,399 bytes, twelve legacy licence headers. GPL version 2
is elected throughout. No vendor bytes were changed; generated platform,
package-boundary, parser-budget and name-alignment adapters live in build
output. The build verifies its pin against the source audit as well as the
actual vendor set. Debugger-only resource dumps are retained but not compiled.

| Exact command (all host jobs sequential) | Result |
| --- | --- |
| `nice -n 19 python3 -B tests/host/acpi/run.py --sanitizer undefined` | PASS: 305 cases, including 277 individually injected allocation failures, DSDT/SSDT and RSDT/XSDT, resources, malformed AML/tables/resources, 64-bit addresses, module/method budgets, EC/SCI denial, registry generations/quarantine, allocation admission, queue/waits/locks, paging and worker creation failure. |
| `nice -n 19 python3 -B tests/host/acpi/compile_target.py` | PASS: canonical i686 flags, 4-KiB frame bound, relocatable ACPICA/OSL link with no unresolved ACPICA symbols, literal v1/v2 ABI, SDK client and kernel integration units. |
| `nice -n 19 python3 -B tests/host/test_devices.py` | PASS: 3 tests; syscall copies/bounds and production Device Manager navigation, five-record paging, stale/error handling and 30 pinned render frames. |
| `nice -n 19 python3 -B tests/host/test_credits.py` | PASS: 8 tests; notice, About rows and pin agreement. |
| `nice -n 19 bash scripts/test/host_kernel_tests.sh` | BLOCKED at ACPICA stage: 305 assertion cases finish without ASan/UBSan findings, then all 305 processes exit 1 because LeakSanitizer cannot inspect threads in this sandbox. Later kernel suites do not run. Leak detection stays enabled. |
| `git diff --check` | PASS. |

The normal synthetic firmware describes ten records (including root and
predefined containers), five validated tables and reaches 277 allocations;
OSL peak is 11,962 bytes on the 64-bit host. Every injected allocation fault
leaves zero tracked allocations and zero mappings. Firmware fixture pages
are protected read-only with mprotect. No unauthorised fake hardware access
occurs. The i686 relocatable aggregate has text=208,806, data=904 and
BSS=52,400 bytes. It includes the 36,352-byte fixed record cache. The syscall
adds a 2,328-byte page buffer; Device Manager retains one page and a cursor.
These are host/compile measurements, not final image or physical-memory
qualification. Registry and v1 capacities, PMM and kheap limits are unchanged.

The Device Manager runner retains its pre-existing leak-detection setting;
the new ACPICA runner never disables it. Its separate UBSan-only run is
supplementary evidence and does not clear the blocked ASan/LeakSanitizer gate.
Intentional golden changes cover the Firmware button and its visible dialog;
the firmware and device-list PNGs were inspected. The approved Ciuki portrait
and icon assets are unchanged.

Lead work remains: review the v2 call-76 ABI and generated interpreter seams;
rerun the complete host entry point where LeakSanitizer can inspect threads;
perform the canonical build and final link-map/image checks. No QEMU,
systemd scope, image build, git write or diary edit was performed here.
Firmware-specific results remain unqualified. SCI_EN and current power values
stay unknown without a qualified owned-region read; no automatic read grant
or power takeover occurs. PnP BIOS node calls/ISA configuration and generic EC,
SCI/GPE, thermal and power activation remain the later directives' work.

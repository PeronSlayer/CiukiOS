# F0-38 loader UART diagnosis and kernel recovery

Primary sources checked on 2026-10-17, before changing code:

- [TI PC16550D datasheet, sections 8.5–8.6](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/PC16550D.pdf):
  SCR is software storage at offset 7. DLAB selects DLL/DLM at offsets 0/1;
  divisor 3 with the PC 1.8432 MHz clock gives 38400 baud. LCR 3 selects
  8N1. LSR bit 5 is THRE, while bit 6 additionally requires an empty shift
  register. A floating-bus FFh value must not count as ready.
- [TI TL16C550D datasheet, register descriptions and baud generator](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf):
  confirms the same register banking and divisor calculation. Clear DLAB
  before disabling IER; restore 8N1 after divisor read-back, including failures.
- [SeaBIOS 1.16.3 serial discovery](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/serial.c):
  `detect_serial` populates `port_com[]` after its own hardware checks.
  BDA COM1 is a firmware discovery result, not an independent hardware test.

Repository comparison: loader `uart_init` previously returned without a
reason on BDA, SCR, LSR or divisor failure. `uart_putc` zeroed the base/divisor
without clearing the serial flag on timeout/budget exhaustion. The kernel
rejected base zero before touching hardware, and its probe did not validate
LSR or divisor read-back. The FIFO off/on/sticky test selects FIFO capacity;
it is not a UART presence test and remains a one-byte fallback on failure.

Decision under the F0-38 directive: name byte 0x00F `uart_state`, preserving
every offset, size and version. Values are 0 unknown (older loaders), 1 OK,
2 no BDA COM1 at 3F8h, 3 SCR failure, 4 LSR FFh, 5 divisor failure,
6 loader THRE timeout, 7 loader character budget exhausted. Disable paths
clear base, divisor and serial flag. The kernel preserves this diagnostic
byte, independently probes COM1 at 38400 8N1 when it is not 1, and records
the original loader state/base/divisor before updating the runtime UART
fields used by the resource registry. Successful recovery sets the serial
flag; failure clears runtime base/divisor and the flag.

Kernel probe result codes: 0 not attempted, 1 OK, 2 unsupported IRQ mapping,
3 SCR failure, 4 LSR FFh, 5 divisor failure, 6 bounded THRE timeout. The
initialization THRE wait is bounded to 4096 LSR reads, in addition to one
initial LSR presence read; normal producers still
perform no UART port I/O. The H-page absence message reports the preserved
loader step and the kernel probe result. The result describes initialization,
not remote delivery or later panic-drain health.

Host commands, results and limitations are recorded in
`build/codex/f0-38-report.md`. QEMU and physical T23 validation belong to the
lead; host tests alone do not identify the cause of the earlier captures.

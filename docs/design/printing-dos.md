# DOS printer capture (G31)

The Windows 98 Resource Kit, chapter 11, “Printing Support for MS-DOS-based
Applications” (`wrkc11_1.htm`, supplied read-only Resource Kit extraction),
specifies that DOS printer-port output joins the same background spooler as
Windows output. DOS output is raw printer data; it does not use EMF spooling.
The [Microsoft Print Spooler API](https://learn.microsoft.com/en-us/windows/win32/printdocs/print-spooler-api)
provides the shared queue model. This is the Windows 95/98 V86 model.

The [SMSC FDC37C665GT/FDC37C666GT data sheet](https://media.digikey.com/pdf/Data%20Sheets/SMSC/FDC37C66xGT.pdf),
pages 92–95, defines SPP data at base+0, status at base+1, control at base+2,
and control bit 0 inverted onto nSTROBE. Therefore captured direct-port data
is accepted on a 0-to-1 transition of control bit 0, once per pulse; merely
writing the data register does not print a byte. Word/dword cycles are not
silently split. Captured SPP is not an EPP/ECP device emulator.

## CiukiOS policy

LPT1 defaults to the native spooler. Explicit per-game Printer mode uses the
same capture; None disables it, Raw preserves the physical exclusive lease,
and File preserves the existing append-to-document route. INT 17h and trapped
SPP writes share one byte stream. BIOS status preserves AL and FLAGS and
reports a timeout when capture cannot accept another byte. No guest can
select a physical lease through registers.

Capture holds at most 512 bytes per VM and streams to the native spool
directory through the VM's host VFS table, not its private DOS disk image.
A form feed, five seconds without a new byte, the shared 16 MiB job limit,
or VM teardown ends a job. Form feeds remain in the raw payload. File I/O
runs at preemptible monitor boundaries or after the VM worker is quiescent,
never from the timer interrupt. Buffer saturation advertises busy/error.

The producer shares the private `apps/libprint/print.h` records with native
and Win32 producers: reserve `C:\WINDOWS\SPOOL\j001` through `j128` with
exclusive mkdir, write `data`, commit it, then write and commit `job.tmp` and
rename it to `job`. The worker ignores unpublished jobs. Capture selects the
configured default printer when publishing. If `printers` is temporarily
absent during replacement, it uses a complete `printers.bak`; a complete
`printers.tmp` supersedes that backup only when the backup proves an existing
configuration is being replaced. A partial temporary record is ignored.
An absent config and backup select printer 0, the native default. Header
selection requires reading the complete fixed config record, streamed in
64-byte chunks, so an unfinished body cannot change the selected printer.
Data is never rendered or text-converted again.
On short writes or I/O errors, remaining buffered bytes and the reserved
slot stay owned by the VM for retry. Automatic failed-I/O retries are limited
to once per second so a guest busy-poll cannot flood the disk. A failed final publication retains the
VM at teardown; the disk table is not closed underneath retained output.
This does not claim power-failure recovery for an unpublished job, physical
printer qualification, or virtual ECP/DMA capture.

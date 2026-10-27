# Win32 printing (G31)

## Reference behaviour and implementation decision

The Windows 98 Resource Kit, chapter 11, `wrkc11_1.htm` (local reference at
`build/rskt`), describes shared 32-bit background printing for Win32 and DOS,
EMF spooling for non-PostScript printers and RAW PostScript spooling. The shared
native spool queue is the authority for installed printers, defaults and jobs.
Win32 must never report a successful document when no queue entry exists.

Microsoft's [GDI printing sequence](https://learn.microsoft.com/en-us/windows/win32/printdocs/using-the-gdi-print-api)
uses CreateDC, StartDoc, StartPage, EndPage and EndDoc, with AbortDoc discarding
the document. [RAW printing](https://learn.microsoft.com/en-us/windows/win32/printdocs/sending-data-directly-to-a-printer)
uses OpenPrinter, StartDocPrinter, page delimiters and WritePrinter.
[EnumPrinters](https://learn.microsoft.com/en-us/windows/win32/printdocs/enumprinters)
requires packed output records and strings, precise required-buffer reporting,
and local printer enumeration at levels 1, 2, 4 and 5.

The existing scalar GDI engine already renders text, shapes and DIBs; G31 adds
printer-owned monochrome page surfaces and exports printer lifecycle functions.
Output goes through the same persistent spool protocol as native applications.
The initial renderer uses a bounded 96 DPI monochrome page and native raster
spool data. It does not implement Windows printer driver DLL loading, EMF spool
files, network print providers or colour/driver-specific DEVMODE options. These
remain explicit compatibility gaps rather than successful placeholders.

Windows pointer-containing SDK layouts retain the i386 ABI. State transitions
reject invalid ordering and abort incomplete documents when the DC/handle is
released. Queue errors propagate as Win32 failures. Enumeration supports ANSI
and Unicode entry points using the same installed native printer state.

## Delivered Win32 boundary

`winspool.drv` forwards its implemented exports to the original GDI producer;
there is one process-local handle table and the shared on-disk queue at
`/WINDOWS/SPOOL`. The compatibility file service protects `/System` against
Win32 writes, which is why the queue belongs to the existing writable Windows
compatibility tree. No kernel syscall or service ABI is added.

The facade implements local EnumPrinters/GetPrinter levels 1, 2, 4 and 5,
default-printer selection, RAW document streaming, per-printer pause/resume/purge,
EnumJobs level 1 and SetJob pause/resume/cancel/delete. Output buffers include
records and strings with an exact byte requirement. Job enumeration snapshots
metadata before packing it, so concurrent queue changes cannot enlarge a
previously sized output record. Unknown network/provider flags and other levels
return explicit failures. [EnumPorts](https://learn.microsoft.com/en-us/windows/win32/printdocs/enumports)
(fx-is5boot; the InstallShield 5 engine imports `EnumPortsA`) lists, for the
local server only (NULL or empty name; any other name is ERROR_NOT_SUPPORTED as
for EnumPrinters), the native queue's output devices `/dev/lpt0` to
`/dev/lpt2` at levels 1 and 2 with exact sizes, spelled as PRINTER_INFO_2/5
report `pPortName` so a program can match the two. Level-2 records name the
`CiukiOS native` monitor, `Printer Port (LPTn)` and PORT_TYPE_WRITE. Print to
file names a path, not a port, so no `FILE:` entry is invented. See Microsoft's [EnumJobs](https://learn.microsoft.com/en-us/windows/win32/printdocs/enumjobs)
and [SetJob](https://learn.microsoft.com/en-us/windows/win32/printdocs/setjob).

GDI printer DCs use 96 DPI logical monochrome pages. PostScript uses the
selected physical PageSize. PCL emits standard 100 DPI raster data with nearest
resampling to preserve the physical page dimensions, plus Letter/Legal/A4 and
orientation commands. Generic Text Only places TextOut characters on an 8 by 16
logical-pixel grid; it does not render
graphics or font-dependent text metrics. Pages share the normal scalar GDI
surface and object lifetime. StartDoc returns the reserved native job identifier,
matching [StartDoc](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-startdoca).
DEVMODEA/W support orientation, Letter/Legal/A4/custom paper dimensions and copies;
other scaling and driver-specific options remain unsupported. Output-file paths
are resolved by GetFullPathName and translated to native UTF-8 paths before the
native worker receives them.

Data and metadata are flushed before publication. The configuration writer uses
the native exclusive `config.lock` directory and owner PID file, then the same
`printers.tmp`/`printers.bak` replacement sequence. Existing locks fail busy;
stale-lock reclamation is owned by the native implementation. Open incomplete
jobs are removed when a handle/DC closes or GDI detaches normally. Abrupt process
death can leave an unpublished directory and requires later queue recovery.

Remaining Win32 compatibility gaps: EMF spooling; Windows printer DLL loading;
DeviceCapabilities, DocumentProperties and GetPrinterDriver; ResetDC/SetAbortProc;
network printer providers; JOB_INFO levels above 1 and job-property edits;
printer-specific colour, duplex, bin, resolution and font options; accurate
Generic Text Only proportional font/alignment/clipping; timestamps/username/page
counts in JOB_INFO_1 (unknown fields are zero/empty); abrupt-death cleanup of
unpublished spool directories. Hardware output and original applications need
lead validation; host tests prove shared queue/worker behaviour and raster data,
not physical printer compatibility.

The [HP PCL5 technical reference](https://www.hp.com/ctg/Manual/bpl13210.pdf),
chapter 15, Raster Graphics Resolution (printed page 15-7 / PDF page 314), lists
75/100/150/200/300/600 DPI. The renderer therefore emits 100 DPI instead of
sending unsupported 96 DPI. The [Adobe PostScript Language Reference](https://www.adobe.com/jp/print/postscript/pdfs/PLRM.pdf),
section 6.2, defines PageSize media selection through setpagedevice. PCL custom
paper selection and physical printer unprintable margins require qualification.

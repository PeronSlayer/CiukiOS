# Native application printing (G31)

The reference is the owner's Windows 98 Resource Kit, chapter 11,
`wrkc11_1.htm`, sections “Overview of the Windows 98 Printing Subsystem”,
“Spooling”, and “Printing from MS-DOS-Based Applications”. Windows 98
queues a document, returns control to the application, and sends data only
as the printer accepts it. DOS output is RAW, separately from GDI rendering.

Microsoft's [Print dialog](https://learn.microsoft.com/en-us/windows/win32/dlgbox/print-dialog-box)
defines the selected printer, copy count and inclusive page range; Cancel
does not submit a job. [Page Setup](https://learn.microsoft.com/en-us/windows/win32/dlgbox/page-setup-dialog-box)
defines paper dimensions, orientation and margins. Native dimensions use
points (72 per inch), with explicit A4 and Letter examples. A printer list
identifies the installed printers; unsupported driver features are
not offered. Print to File has an explicit destination.

CiukNote, Paint and Image Viewer expose File menu commands. A callback from
the application model reaches a per-window printing controller in its native
host. Opening Print copies the unsaved text or full image, independent of the
screen's zoom and scroll. Confirming the dialog renders and publishes this
immutable snapshot on a worker; Cancel frees it. The application polls the
completion and reports either the queued job identifier or the actual error.
The controller joins its worker before the application frees its storage.
Page Setup changes the controller's settings only on OK. No document is
saved or marked clean by printing.

NetSurf uses the same dialogs from Ctrl+P and Ctrl+Shift+P. The native
frontend's current framebuffer supplies a viewport snapshot; this is explicitly
a visible-page image, not full-document pagination or CSS paged-media output.
Full browser document printing remains a gap until an upstream print plotter
and its pagination engine are connected.

Shared queue, driver and retry semantics are specified in [printing.md](printing.md).
There are no new system calls or desktop protocol messages.

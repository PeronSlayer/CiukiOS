# Part C implementation decisions

The binding semantics are in
[display-command-semantics.md](../../design/display-command-semantics.md),
including its appended lead decisions. The Part A base omitted that document;
it is copied verbatim from the approved lead checkout for this implementation.

Primary-source comparison:

- [Windows 98 Resource Kit, chapter 24](</home/peronslayer/Desktop/CiukiOS/build/rskt/wrkc24_3.htm>),
  Display Driver Overview: the common DIB engine handles memory and
  unaccelerated screen operations. Keep the desktop software cursor owner.
- [Haiku Accelerant.h, master, headers/os/add-ons/graphics/Accelerant.h](https://raw.githubusercontent.com/haiku/haiku/master/headers/os/add-ons/graphics/Accelerant.h):
  separate cursor/drawing hooks, engine ownership and completion tokens;
  scaled blits forbid overlap. Preserve this separation with synchronous
  whole-record hooks. No Haiku code was copied.
- [X.org EXA exa/exa.h, master](https://raw.githubusercontent.com/mirror/xserver/master/exa/exa.h):
  preparation refusal precedes submission and CPU access requires ordering.
  A whole-record hook avoids a false clean refusal after earlier fragments.
- [Linux v6.12 include/linux/fb.h](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/fb.h)
  and [drivers/video/fbdev/core/cfbimgblt.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/video/fbdev/core/cfbimgblt.c):
  separate image expansion/cursor/sync; explicit foreground/background,
  MSB-left rows and padding. The Ciuki arithmetic is original code.
- [Linux master cfbfillrect.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/core/cfbfillrect.c):
  synchronize before CPU stores. Synchronize both endpoints, drain CPU stores
  before native access and completion, and retain unsafe drawing ownership.
- [Microsoft LineTo](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-lineto),
  [DrawFocusRect](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-drawfocusrect),
  [GDI raster operations](https://learn.microsoft.com/en-us/windows/win32/gdi/ternary-raster-operations)
  and [X11 PolyLine](https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html):
  endpoint exclusion, repeated XOR and clipping-invariant rasterization.
  Ciuki's explicit ties and bilinear arithmetic follow the approved contract.
- [Windows 95 DDK minidriver guidelines](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S27F2.HTM)
  and [design guide](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2733.HTM):
  DIB-engine dispatch with optional minidriver acceleration. These are 9x
  references; the implementation does not substitute NT display architecture.

`DISPLAY_ACCEL` operations 10/11/12/13 negotiate batches, fence controls,
command capabilities and managed-surface identity. Existing requests 0–9 and records retain their binary layouts.
The envelope travels in EDX, as the existing negotiated query output does.
The SDK and ABI oracle are updated together.

Managed surfaces use new `SURFACE_CREATE` formats 2/3/4, RGB565/BGR888/XRGB8888.
They have immutable geometry and allocation generations and cannot be mapped
into user space. `DISPLAY_ACCEL/QUERY_SURFACE` returns the versioned
identity plus legacy geometry. Legacy `SURFACE_INFO` still writes exactly 24
bytes and ignores omitted registers, as execution-abi.md requires. Existing mapped surfaces fail batch admission
because mapped writers cannot be excluded. Backing uses resident PMM pages,
not the kernel's 2040-byte heap; allocation failures precede drawing. These
are request allocations, not changed global memory limits or reservations.
Kind 2 uses the owned display fd as a handle to its complete, already-verified
native offscreen lease. Its generation and span are rechecked under presenter
admission; the retained in-flight owner pins that driver lease across slices
and recovery. It is always engine-only, never a guessed CPU mapping. The
snapshot does not run pixel probes. Native family implementations are outside
this directive.

Host evidence and exact commands are recorded in
`build/codex/f2-101d-report.md`. No image or hardware evidence is claimed.

The cursor raster structure accepts the f2-99 shadow plane. The Part A raster
set is unchanged and uses a null shadow. When integrated with f2-99's generated
header, the same ARGB builder emits opaque black/white plus premultiplied black
shadow directly from its exact masks and alpha data. The standalone host client
test can read that header from the lead checkout without copying its artwork.

Client batches use MONO for binary text/rows, INVERT for verified XOR changes,
and KEYED for changed pixels uploaded through a row reserved from legacy
video caches. A temporary snapshot is limited to one small damaged region
(64 KiB); unavailable or unsuitable regions retain the normal upload path.
Keys are selected after the current channel quantization. No complete second
frame is allocated. The client never resubmits a failed non-idempotent batch;
it retains a recovery fence and reconstructs from authoritative RAM only
after CPU safety. Every terminal fence is released.

CPU execution checks an eight-sample boundary and each completed record.
With calibrated TSC, the presenter yields when a half-millisecond work slice
is due; without TSC it yields at each boundary. TSC is only a pacing estimate,
not a completion proof. CPU stores drain before every release and completion;
native waits keep their existing one-millisecond slicing and retained claim.

Preflight coalesces referenced payload ranges before checking zero padding;
validation yields outside display locks. Geometrically empty records complete
without native submission and cannot establish native-used evidence. Recovery
has one proof owner across sliced waits. The kernel worker retains live and
closed-grant jobs, including source objects, staging and reservations, until
idle/stop proof; no drawing record is queued or replayed by that worker.

The f2-99 shadow overlay uses decoded target RGB before alpha composition,
matching the approved 16-bit cursor semantics as well as the 32-bit path.
Existing opaque cursor rasters retain their pixels. The client fixture compares
software and native-overlay pictures at RGB565 and XRGB8888 with the lead's
exact shadow header.

The display-model link to Microsoft's “Copying bitmaps” page was unavailable
through the research tool. [EngCopyBits](https://learn.microsoft.com/en-us/windows/win32/api/winddi/nf-winddi-engcopybits)
was checked as supporting NT dispatch/format evidence. The Windows 9x
architecture remains grounded in the Resource Kit and archived DDK above.

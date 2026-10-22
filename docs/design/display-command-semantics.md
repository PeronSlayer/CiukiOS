# Display command semantics

Status: proposed part C specification, 2026-10-23; requires lead cross-review
before implementation. Written for directive f2-101c. This document supplements
[the display model](display-driver-model.md), especially Amendment 1, items
1, 4–8 and 11. It does not authorize part C code or change part A's scope.
The rules below are normative for the proposed version 1; numeric syscall,
request and capability allocations belong to the later implementation directive.
Legacy operation numbers, capability bits, the 308-byte request, the 40-byte
query and WRITE's 0/1 results remain unchanged.

## Evidence and decisions

The sources establish the mechanisms in this table. The exact arithmetic,
wire protocol, limits and scheduling below are CiukiOS design decisions, not
claims that these systems have identical ABIs or pixels.

| Primary reference inspected | Evidence and resulting decision |
| --- | --- |
| [Haiku `Accelerant.h`](https://github.com/haiku/haiku/blob/master/headers/os/add-ons/graphics/Accelerant.h), engine, cursor and 2D hooks | Hook lists include inversion, transparent and scaled filtered blits, cursor replacement and synchronization tokens. The scaled hook explicitly forbids overlap. Adopt separate operations and forbid SCALED overlap; do not infer a filter kernel or ARGB alpha convention from this header. |
| [X.org EXA `exa.h`, upstream source mirror](https://github.com/mirror/xserver/blob/master/exa/exa.h), `PrepareCopy`, `Copy`, `MarkSync`, `WaitMarker`, `PrepareAccess` | Preparation can refuse into software; copy directions handle overlap; CPU access needs synchronization. Preserve these distinctions, including synchronization of sources as well as destinations. |
| [Linux v6.12 `fb.h`](https://github.com/torvalds/linux/blob/v6.12/include/linux/fb.h), `fb_ops` | Fill, copy, image expansion, cursor and synchronization are separate operations. Keep a software implementation independent of hardware capability reporting. |
| [Linux v6.12 `cfbimgblt.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/core/cfbimgblt.c), `slow_imageblit`, `fast_imageblit`, `cfb_imageblit` | One-bit images select foreground/background, round rows to bytes, consume high bits first, and synchronize before CPU writes. Adopt MSB-left, byte-padded rows and explicit stride. |
| [Linux `cfbfillrect.c`](https://github.com/torvalds/linux/blob/master/drivers/video/fbdev/core/cfbfillrect.c), current wrapper | Calls `fb_sync` before the shared fill helper. This is evidence for the synchronization boundary, not for CiukiOS's mask or rounding rules. The current wrapper differs from older inline fill implementations. |
| [Microsoft GDI ternary raster operations](https://learn.microsoft.com/en-us/windows/win32/gdi/ternary-raster-operations) | ROPs are Boolean combinations of source, pattern and destination. Version 1 supports only the operations explicitly listed below, rather than accepting arbitrary ROP3 integers. |
| [Microsoft `LineTo`](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-lineto) | Excludes the final endpoint. Choose that GDI convention to compose adjacent segments without drawing a shared vertex twice. This modern reference supports the convention, not a claim of measured Windows 9x diagonal tie behavior. |
| [X11 protocol, CreateGC and PolyLine](https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html) | Zero-width rasterization is device dependent, must be translation invariant and unaffected by clipping; NotLast omits the endpoint. Choose explicit arithmetic instead of device-dependent diagonals, while preserving clipping invariance. |
| [Microsoft `DrawFocusRect`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-drawfocusrect) | A repeated XOR removes the outline. Normalize a clip union before inversion; each pixel is inverted once per record. INVERT is a filled region primitive, not the whole DrawFocusRect API. |
| [Archived Windows 95 DDK, Display Minidriver Guidelines](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S27F2.HTM) and [Design and Implementation Guide, Display Drivers](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2733.HTM) | Microsoft's archived text makes the DIB engine the common GDI implementation and adds hardware acceleration/cursors to the minidriver. Use the software engine as the semantic authority; these are Windows 95 sources, not NT's display DDI. |

The locally readable Windows 98 Resource Kit, chapter 24, “Display Driver
Overview” (`build/rskt/wrkc24_3.htm` in the lead checkout), also describes
in-memory operations and unaccelerated on-screen operations going to the DIB
engine. No private media or extracted book text is added to this repository.

Repository comparison at the directive's base state:

| Existing implementation | Constraint on part C |
| --- | --- |
| `src/kernel/drivers/fbdev.c`: `layout_valid`, `pack_layout`, `pixel` | Validated 16/24/32-bit channel masks, high-bit truncation from RGB8, little-endian packed pixels and zero reserved bits on colour stores. Preserve that conversion. |
| Same file: PATTERN branch in `fbdev_accel_request` | Absolute destination coordinates select MSB-left pattern bits. Legacy wrappers use pattern origin (0,0). |
| Same file: `accel_pause`, `service_begin`, `service_end`, `drawing`, `accel_inflight` | Presenter mutex release is not ownership release. Keep the drawing claim and in-flight state across every sliced wait. |
| `src/kernel/proc/grants.c`: `grant_check`, `grant_accel`, `accel_write` | Display grant, process ownership, retained descriptions and faultable copies outside display locks are existing boundaries. Legacy WRITE stages bands; a new batch must stage all its upload bytes before drawing to provide the stronger preflight guarantee. |
| `src/kernel/include/ciuki/accel.h`, `abi.h` | Existing hooks and requests remain binary compatible. Native cursor absence means desktop overlay, not a hidden kernel software cursor. |

## Common pixel and geometry rules

Coordinates are signed integer pixel indices with x rightwards and y
downwards. Rectangles are `(x,y,w,h)` and half open: `x <= X < x+w`,
`y <= Y < y+h`. Widths/heights are unsigned. All sums, products, range ends
and address calculations use checked arithmetic, with at least 64-bit
intermediates; no signed overflow, wraparound or pointer-order alias tests.
Only LINE interprets its two coordinate pairs as endpoints.

Targets have immutable width, height, pitch, allocation extent, RGB channel
sizes/positions and bpp. Pixels occupy exactly bpp/8 bytes, including at 24
bpp. Pitch may include padding; padding and bytes outside affected pixels
are never touched. RGB channels have 1–8 bits each, fit inside 16/24/32 bpp
and do not overlap. Reserved bits may occupy the remaining positions.
Indexed, planar, negative-pitch, tiled and alpha destination formats are
unsupported. Validate full metadata before accessing even a clipped target.

Colour arguments are `0x00RRGGBB`; their high byte must be zero in the new
wire format. WRITE rows are `0xXXRRGGBB` and ignore X, preserving legacy
upload meaning. For an 8-bit channel c, a destination channel of n bits at
position p receives `(c >> (8-n)) << p`. This is truncation, not rounding
to nearest. Define `M` as the union of the three RGB masks and `P(rgb)` as
their packed sum. Colour stores write `P(rgb)` with all bits outside M zero.
To decode an n-bit channel v for scaling or cursor blending, use
`floor((255*v + floor((2^n-1)/2))/(2^n-1))`. There is no gamma correction,
dithering or colour management. Operations work in encoded RGB space.

| Canonical test format | Masks (size,position) R/G/B | Native colour and stored bytes for RGB `123456` |
| --- | --- | --- |
| RGB565, 16 bpp | (5,11)/(6,5)/(5,0) | `11aa`; `aa 11` |
| BGR888, 24 bpp | (8,16)/(8,8)/(8,0) | `123456`; `56 34 12` |
| XRGB8888, 32 bpp | (8,16)/(8,8)/(8,0) | `00123456`; `56 34 12 00` |

These are test formats, not a restriction on valid VBE RGB positions.
For example RGB555 in a 16-bit word is supported with M=`7fff`.
COPY/KEYED require identical bpp and RGB masks; otherwise refuse with
`-EOPNOTSUPP`. WRITE and SCALED perform the stated conversion. No operation
implicitly alpha blends except colour cursor composition.

### Clip union and source mapping

Each drawing record has 0–16 destination rectangles. Count zero means no
extra clipping. A nonzero list is the set union, intersected with destination
bounds and the operation's footprint. Rectangles may be unsorted, repeated,
empty, overlapping or outside bounds. Empty entries contribute nothing; a
list of only empty entries produces no pixels. Invalid extents or arithmetic
overflow are refused, even for entries outside bounds.

Evaluate each destination pixel once per record. An implementation may
normalize to disjoint row spans; it must not draw each input rectangle
independently. This is essential for INVERT and also for overlap-safe COPY.
Separate records are separate applications: two INVERT records restore RGB.
For focus borders the caller supplies disjoint border geometry or independent
records that avoid double-inverting corners.

Clipping only removes destination samples. It never shifts the original
source origin, restarts a line, recenters scaling or restarts a pattern.
For unscaled transfers, destination `(X,Y)` samples
`(sx+X-dx, sy+Y-dy)`. Samples outside source bounds are omitted for COPY,
KEYED, WRITE and MONO. SCALED requires its full source rectangle in bounds
before clipping, including for a completely clipped destination.
Zero width or height is a successful no-op after ordinary validation; no
payload bytes are needed for that empty upload. Such records still count as
completed. SCALED's source and destination must either all be nonzero or
have an empty destination; a nonempty destination with empty source is invalid.

## Command definitions

Numbers below are local to batch version 1, not legacy operation numbers.
All commands use the common conversion, clipping, validation and refusal
rules. Fields not listed in the wire-field table must be zero.

| Opcode | Command | Pixel operation and supported source |
| --- | --- | --- |
| 1 | FILL | Store `P(foreground)` throughout the destination rectangle. |
| 2 | COPY | Copy each sampled source's RGB bits, clearing destination reserved bits. Values are those at this record's start, even for overlap. |
| 3 | PATTERN | Repeat the inline 8×8 one-bit pattern. A one stores `P(foreground)`, a zero stores `P(background)`. No transparent pattern mode in v1. |
| 4 | WRITE | Expand staged XRGB8888 upload rows into destination pixels. No alpha, no key, no arbitrary ROP. |
| 5 | MONO | Expand staged one-bit rows. One stores foreground; zero stores background unless TRANSPARENT_BG is set, in which case that pixel, including reserved bits, is unchanged. |
| 6 | INVERT | Store `D XOR M` at each eligible pixel. Preserve reserved bits exactly. No source or colour argument. |
| 7 | KEYED | Compare `(S & M)` with `P(key)` in the source format, before copying. Equal means preserve the entire destination pixel; otherwise copy RGB and zero reserved bits. |
| 8 | SCALED | Sample the source target into the destination rectangle using NEAREST or BILINEAR below, then pack into the destination format. No alpha, key or mirroring. |
| 9 | LINE | Store foreground at the single-pixel samples below; COPY raster operation only in v1. |
| 10 | SYNC | No pixels; complete all prior batch work and drain CPU framebuffer writes before reporting this record complete. No target or clip list. |
| 11 | CURSOR_MONO_SET | Replace native cursor with two 32×32 AND/XOR planes and packed foreground/background as defined below; hide before successful replacement. |
| 12 | CURSOR_ARGB_SET | Replace native cursor image, up to 64×64, with the premultiplied ARGB definition below; hide before successful replacement. |
| 13 | CURSOR_MOVE | Set hotspot's signed screen position, including partially/wholly offscreen positions. No framebuffer write. |
| 14 | CURSOR_SHOW | SHOW flag selects visible; zero hides. A successful SET leaves hidden, so showing a replacement requires this separate command. |

### Bit order and pattern origin

MONO source `(u,v)` is `(bytes[v*stride + floor(u/8)] >> (7-(u%8))) & 1`.
Source coordinates are nonnegative after bounds checks. The stride is at
least `ceil(source_width/8)`; unused tail bits and row padding are ignored.
Rows are top to bottom. The upload includes all declared rows, including
padding between them, with minimum extent `(height-1)*stride+row_bytes`.

PATTERN stores eight bytes in top-to-bottom order, bit 7 leftmost. Its bit
at destination `(X,Y)` is
`(pattern[mod8(Y-origin_y)] >> (7-mod8(X-origin_x))) & 1`, where mod8 is
Euclidean modulo in [0,7], also for negative arguments. Origin is relative
to the target, not the rectangle or clip. Legacy wrappers set both origins
to zero. PATTERN and MONO do not share an implicit origin.

### Aliasing and overlap

Overlap is determined from underlying allocation identity and byte ranges,
including aliases through different handles, mappings or offsets. Comparing
virtual addresses or target indices alone is insufficient. Reject uncertain
aliasing before submission. The conservative test uses full in-bounds source
and destination rectangles before extra clip lists, so clipping cannot make
a forbidden overlapping command acceptable.

| Command | Source/destination alias rule |
| --- | --- |
| FILL, PATTERN, INVERT, LINE | No external source; inline colours/pattern are already staged. |
| COPY | Allowed on the same target/view with identical pitch and layout, in all directions. Conceptually snapshot the source at record start. Globally ordered traversal of the clip union, or preallocated scratch, must preserve this result. Distinct views of one allocation with different bases/pitches are refused with `-EINVAL` if the accessed byte ranges overlap. |
| KEYED, SCALED | Any intersecting source/destination byte ranges are forbidden, including exact self-copy: `-EINVAL`. This follows Haiku's scaled restriction and gives keyed copying one simple testable rule. Disjoint rectangles in one allocation are legal. |
| WRITE, MONO, cursor SET | Payload is an immutable kernel snapshot before the first record; later changes to caller memory cannot affect execution. User mappings of video memory or managed batch targets cannot be wire/upload buffers; refuse with `-EACCES` before reading them. |
| SYNC, cursor MOVE/SHOW | No source/destination pixels in storage. |

COPY must preserve a single snapshot across disjoint clip spans. Performing
separate memmoves in clip-list order is not sufficient. When scratch is
necessary, reserve it during preflight before the first pixel; allocation
failure must not occur after part of this record has been drawn.

### Scaling

Let the original source size be `(sw,sh)` and destination size `(dw,dh)`.
For a destination sample, set `i=X-dx`, `j=Y-dy`; do not renormalize these
after clipping. NEAREST (filter=0) uses
`u=floor((2*i+1)*sw/(2*dw))`, `v=floor((2*j+1)*sh/(2*dh))`, with samples
relative to `(sx,sy)`. Exact ties choose the greater source index.
This is center sampling, not `floor(i*sw/dw)`.

BILINEAR (filter=1) uses the source-center coordinate
`qx=(2*i+1)*sw-dw`, denominator `ax=2*dw`; let `l=floor(qx/ax)` and
`rx=qx-l*ax`. Similarly `t=floor(qy/ay)`, `ry=qy-t*ay`, `ay=2*dh`.
Clamp each of `l,l+1` to [0,sw-1] and `t,t+1` to [0,sh-1] independently.
Decode the four pixels to RGB8. For each channel compute the weighted sum
with weights `(ax-rx)*(ay-ry)`, `rx*(ay-ry)`, `(ax-rx)*ry`, `rx*ry` in
top-left, top-right, bottom-left, bottom-right order. Divide by `ax*ay`
with one final round-to-nearest, ties upwards:
`floor((sum+ax*ay/2)/(ax*ay))`. Then pack by truncation. Do not round
intermediate horizontal results. Widen before multiplication; the limits
below make the weighted sum fit uint64. No area averaging on downsampling,
anisotropic filtering or gamma-linear interpolation is implied.

Both filters are required in the software engine. Native filtering is
advertised separately and must produce exact golden pixels or refuse before
submission; an approximate hardware “filtered” mode is not sufficient.

### Lines

LINE chooses GDI's start-inclusive/end-exclusive convention. Coincident
endpoints draw nothing. Rect fields carry `(x0,y0)` and `(x1,y1)` as
`(dx,dy)` and `(sx,sy)`; widths/heights are zero. Set
`N=max(abs(x1-x0),abs(y1-y0))`. For each integer k in [0,N):

- If `abs(x1-x0) >= abs(y1-y0)`, x is `x0+sign(x1-x0)*k`, and y is
  `floor((2*(y0*N+(y1-y0)*k)+N)/(2*N))`.
- Otherwise y is `y0+sign(y1-y0)*k`, and x is
  `floor((2*(x0*N+(x1-x0)*k)+N)/(2*N))`.

Floor is mathematical floor, including negative numerators. This fixes
halfway diagonals toward the greater minor coordinate, independent of
traversal direction. Equal major-axis magnitudes use x. Endpoints are not
clipped and rerasterized: generate the original sample set then mask it.
An optimized skip-to-visible implementation must give the same samples.
This specifies CiukiOS ties; it does not assert undocumented GDI ties.
X11 zero-width lines deliberately permit device-dependent ties, so merely
calling an X-style native hook would not establish conformance.

### Cursor pixels and ownership

Cursor operations affect the native overlay plane only and require the
active display grant. They never modify target backing pixels. No clip list
is allowed: scanout bounds clip the overlay. Their target is current scanout.
The software engine supplies cursor composition goldens for the desktop,
but does not install a kernel cursor fallback. With no conforming native
cursor, refuse with `-EOPNOTSUPP`; the desktop owns its software overlay.

MONO_SET payload is 128 bytes of AND rows followed by 128 bytes of XOR
rows. Both use four bytes per row, MSB-left, 32 rows. At one pixel:
AND/XOR 00 selects background, 01 selects foreground, 10 leaves the underlying
screen unchanged, 11 XORs its RGB with M (preserving reserved bits).
This is a four-state monochrome cursor, not arbitrary RGB AND/XOR operands.
Hotspot is (0,0) for the legacy wrapper; v1 permits each coordinate [0,31].
The legacy request instead supplies an opaque mask (one means opaque) and
an image selecting foreground. Its wrapper translates to AND=`~mask`,
XOR=`image & mask`; image bits outside the opaque mask remain ignored.
It must not reinterpret old binaries' mask bits as AND bits. A native hook
that implements only transparent/background/foreground must refuse the new
inversion state before replacing the cursor; legacy supported shapes remain
usable through their existing path.

ARGB_SET uses width/height in [1,64], constrained further by the negotiated
driver maximum. Payload is top-down packed little-endian `0xAARRGGBB`
(bytes B,G,R,A), stride a multiple of four and at least width*4. Input is
**premultiplied**: each RGB channel must be <= A; A=0 requires RGB=0.
Reject malformed pixels with `-EINVAL` before hiding the old cursor.
Hotspot must be inside the image. No straight-alpha flag is accepted in v1.

For a premultiplied channel s, alpha a and decoded underlying RGB8 channel d,
composition is `min(255, s+floor((d*(255-a)+127)/255))`, then destination
packing. Alpha 0 leaves the entire underlying pixel unchanged; alpha 255
replaces RGB. The displayed output has no stored alpha. The driver must
implement these pixels at the current bpp or refuse; a binary-alpha-only
cursor cannot advertise this capability. SET first validates/stages the
entire replacement, then hides and replaces it; success leaves it hidden.
MOVE/SHOW require an installed shape. No installed shape is `-EINVAL`.
Stop/quarantine disables/hides the native plane before software ownership;
an unproved disable is unsafe and must not trigger software cursor drawing.

## Targets, permissions and CPU safety

A batch uses a table of retained target objects, not arbitrary pointers,
physical addresses or unvalidated off-screen row numbers. Target kind 1 is
scanout, kind 2 a leased video-memory target, kind 3 a system-memory surface.
Kind 2 requires its lease owner and generation. Kind 3 resolves a surface fd
with immutable validated geometry/layout, a CPU mapping covering its extent,
and the required read/write rights; creating/negotiating those surface
formats is an interface dependency for the later part C directive.

System-memory operations use exactly the same pixel semantics, padding
rules and overlap tests. No native DMA to pageable user memory is implicit.
The initial implementation uses pinned kernel-visible system surfaces and
CPU rendering; an asynchronous DMA target requires a separate contract.
All clients must refrain from concurrent direct CPU access while their
surface is reserved for a batch. Kernel-mediated access blocks; protection
and backing-object alias checks must prevent untrusted mapped writers from
violating another client's reservation. If this cannot be enforced for a
surface, it cannot be accepted as a batch target. Raw user destination
buffers are not accepted and are never copied back after partial rendering.

Every v1 batch, including a system-only batch, requires an owned writable
DESKTOP_DISPLAY grant, as the current acceleration entry does. Source targets
need read rights; destinations need write rights and, for INVERT or overlap
COPY, read rights. Cursor composition authority includes scanout reading by
the native plane. Cross-process target handles cannot bypass rights checks.
Retain backing objects, fd descriptions, layout and lease references through
completion proof, not merely through enqueue or mutex release.

A CPU read or write waits for all conflicting prior engine reads/writes on
**both** source and destination. CPU framebuffer writes drain before engine
submission, completion publication and release of ownership. A driver
`-EOPNOTSUPP` is legal only before any submission, pixel or cursor change.
Only then may dispatch synchronize conflicts and run software. Quarantine
alone proves no CPU safety: `cpu_ready` is a cached, nonwaiting permission,
not a substitute for proving idle/stop. An engine-only revoked or missing
lease cannot become a guessed CPU mapping. Return an error and reconstruct.
No new panic path or waiting in console/panic drawing is permitted.

Native eligibility/refusal is assessed for the entire record, including its
normalized clip union. Once any fragment has been submitted, a refusal on a
later fragment is a post-submission failure of that record, not permission
to replay it in software. A dispatcher that splits records into legacy hook
calls must track this boundary explicitly.

## Batch wire protocol, version 1

All multibyte fields are little endian. Offsets are byte offsets, never C
pointers. Decode explicitly or use fixed-width layouts with asserted sizes
and offsets on i686 and host builds; no native `size_t`, enum or pointer
fields. Reserved fields, flags and unused command fields must be zero.
Version/size negotiation happens before reading variable-length contents.

Submission envelope (32 bytes), supplied through a new negotiated request on
the existing display-grant fd:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | size=32 |
| 4,6 | u16,u16 | version=1, flags=0 |
| 8,12 | u32,u32 | input user address, input byte count |
| 16,20 | u32,u32 | reply user address, reply size=64 |
| 24 | u32[2] | zero |

Addresses are i386 user virtual addresses; range-check address+length using
widened arithmetic. No 64-bit pointer extension is hidden in a reserved field.
The envelope, blob and reply are disjoint user ranges. Prefault, validate and
pin a writable no-fault reply before any execution. It must not alias target
backing pages. No faultable output copy is allowed after pixels change; the
kernel fence record remains authoritative if the process exits.
Before reading even the envelope, verify that wire buffers use ordinary user
RAM, excluding video apertures and managed surface/target mappings. This
check prevents a staging copy itself from reading an unsafe engine target.

Blob header (64 bytes):

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | magic `0x31424443` (bytes `C D B 1`) |
| 4,6 | u16,u16 | version=1, header_bytes=64 |
| 8,12 | u32,u32 | total_bytes, record_count |
| 16,20 | u32,u32 | records_offset=64, targets_offset |
| 24,28 | u32,u32 | target_count, payload_offset |
| 32,36 | u32,u32 | payload_bytes, flags=0 |
| 40 | u32 | expected current display generation |
| 44 | u32[5] | zero |

Records immediately follow the header. Targets immediately follow the
records: `targets_offset=64+128*record_count`. Payload immediately follows
the targets: `payload_offset=targets_offset+32*target_count`.
`total_bytes=payload_offset+payload_bytes`, is a multiple of eight, equals
the envelope's input byte count, and is at most 1,048,576. There are 1–64
records and 0–128 target descriptors. Only SYNC can use no targets. Unused
payload padding bytes must be zero. Data and clip payload ranges must be
within the payload region and start on eight-byte boundaries; referenced
read-only ranges may be shared by records. No relative-offset loops or
references into headers, records or targets are permitted.

Target descriptors are 32 bytes: at offsets 0/4/8/12 are u32 kind, handle,
generation, flags=0; offsets 16–31 are zero. A record's target reference is
one-based; zero means absent. Kind 1 has handle=0 and generation equal to
the header; kind 2 has a lease handle and that lease's generation; kind 3
has a surface fd and its allocation generation. All descriptors are
validated, including unused ones. Format/size/pitch come from the object,
never this table. All references to one backing object share one reservation.

Command record (128 bytes):

| Offset | Type | Field |
| --- | --- | --- |
| 0,2 | u16,u16 | opcode, flags |
| 4 | u32 | record_bytes=128 |
| 8,12 | u32,u32 | dst target index, src target index |
| 16,20 | s32,s32 | dx, dy |
| 24,28 | u32,u32 | dw, dh |
| 32,36 | s32,s32 | sx, sy |
| 40,44 | u32,u32 | sw, sh |
| 48,52,56,60 | u32 × 4 | foreground, background, key, filter |
| 64,68 | u32,u32 | clip_offset, clip_count |
| 72,76,80 | u32 × 3 | data_offset, data_bytes, stride |
| 84,88 | s32,s32 | origin_x, origin_y (or cursor hotspot) |
| 92 | u32 | zero |
| 96 | u8[8] | pattern |
| 104 | u32[6] | zero |

Each clip entry is 16 bytes: s32 x,y; u32 w,h. Clip count zero requires
clip_offset=0. Absent data requires offset/bytes/stride=0. Data byte counts
equal the minimum extent defined for the upload (zero for empty uploads),
not arbitrary extra bytes. All unspecified fields below are zero, except
record_bytes/opcode and the common destination clip fields on drawing ops.

| Command | Used fields besides opcode/record_bytes |
| --- | --- |
| FILL | dst, dx/dy/dw/dh, foreground, clip |
| COPY | dst/src, dx/dy/dw/dh, sx/sy, clip (source size equals dw/dh) |
| PATTERN | dst, dx/dy/dw/dh, foreground/background, origin_x/y, pattern, clip |
| WRITE | dst, dx/dy/dw/dh, sx/sy/sw/sh, data, stride, clip; sw/sh describe the upload image, stride >= sw*4 and multiple of 4 |
| MONO | WRITE geometry/data fields, foreground/background, clip; flag bit 0 TRANSPARENT_BG; stride >= ceil(sw/8) |
| INVERT | dst, dx/dy/dw/dh, clip |
| KEYED | COPY fields plus key |
| SCALED | dst/src, dx/dy/dw/dh, sx/sy/sw/sh, filter, clip |
| LINE | dst, dx/dy/sx/sy, foreground, clip |
| SYNC | none |
| CURSOR_MONO_SET | dst=scanout, foreground/background, data_offset, data_bytes=256, stride=4, dw=dh=32, origin_x/y=hotspot |
| CURSOR_ARGB_SET | dst=scanout, dw/dh, origin_x/y=hotspot, data_offset/data_bytes/stride |
| CURSOR_MOVE | dst=scanout, dx/dy |
| CURSOR_SHOW | dst=scanout; flag bit 1 SHOW |

Flags have meaning only for their listed command. Cursor SET dw/dh do not
define a drawing rectangle. Upload sx/sy select an offset within the declared
image; row bytes/extent validation uses sw/sh, not the clipped width/height.

### Admission, staging and refusal

Version 1 limits each target dimension and each rectangle width/height to
16,384; LINE coordinates and all rectangle/origin coordinates to
[-1,048,576,1,048,576]. A batch may test at most 4,194,304 destination
samples, summed **before** bounds/clip reduction (LINE contributes N,
cursor SET contributes its area; SYNC/MOVE/SHOW contribute zero). Each target
extent is at most 64 MiB. These are per-request admission limits, not new
global display-memory reservations or changed hardware memory limits.
The 1 MiB blob limit includes every upload and clip byte. Large uploads must
be split into batches; legacy calls retain their existing limits.

Preflight proceeds without a presenter or driver lock:

1. Validate envelope and writable reply; check display-grant type, owner,
   write rights, suspension and caught cancellation/signal.
2. Copy the bounded blob once to kernel staging. Decode the snapshot;
   validate every record, reserved field, target reference, clip/data range,
   pixel format, premultiplied cursor pixel and arithmetic operation.
3. Retain and resolve all target objects with permissions, allocation
   identity, extents, generation, CPU mappings and overlap checks. Acquire
   stable pins outside display locks. Allocate bounded scratch, fence state
   and cleanup bookkeeping. No disk I/O or faultable user pointer is passed
   to a driver. Check full source extents even when clips remove all pixels.
4. Enter the display admission path; serialize target reservations by stable
   object id, never user fd order. Under the presenter mutex recheck grant
   authority, mode generation, suspension, target/lease revocation, quarantine
   and CPU safety. If any fails, unwind without drawing.

Malformed record k therefore rejects the **entire batch** with completed=0;
it is not a reason to draw the earlier records. Completed-prefix behavior
applies to runtime failures/cancellation after successful admission. Staging
is a snapshot, not an atomic read of concurrently changing user memory;
the caller must not modify it during the copy. All subsequent decoding uses
that snapshot. No page fault, allocation, VFS/ATA call or user copy may occur
under a display lock or while a record has partially executed.

| Error | Meaning before the first affected pixel |
| --- | --- |
| `-EINVAL` | Bad magic/size/layout/flags/reserved fields, bad rectangle/stride, invalid alpha/hotspot, forbidden alias/overlap, absent cursor shape. |
| `-EOPNOTSUPP` | Unsupported version, opcode, format, filter or native cursor semantics. A driver's per-call refusal has the stronger no-submission guarantee. |
| `-EOVERFLOW` | Arithmetic or representable address-end overflow. |
| `-E2BIG` | Count, byte, dimension, coordinate, extent or sample budget exceeded. |
| `-EFAULT` | Inaccessible input or unpinnable/inaccessible reply; never a post-drawing user-copy error. |
| `-EBADF` / `-EACCES` | Invalid fd/object kind / missing ownership or access rights, including forbidden upload memory. |
| `-ENODEV` | No active display target. |
| `-EAGAIN` | Stale display/lease/allocation generation, revoked lease, or absent validated mapping; caller reacquires/reconstructs. |
| `-EBUSY` | Mode transition, suspension, occupied admission path or conflicting reservation. |
| `-ENOMEM` / `-ENOSPC` | Staging/pins/scratch unavailable / fence quota or sequence space exhausted. |
| `-EINTR` / `-ECANCELED` | Signal / cancellation before submission. |

If several checks fail, the admission sequence determines the error: grant
and envelope checks first, then structural blob checks, then records in
index order and target checks, then locked lifecycle rechecks. An invalid
range inside a valid bounded blob is a layout error; arithmetic overflow
in computing that range is `-EOVERFLOW`. No-op records still validate their
used fields, rights, format and alias rules; an empty destination's upload
has data_offset/data_bytes/stride zero and needs no user payload.

Each **refused** record has no pixel or cursor change, and no engine work
submitted. The same applies to a preflight-refused batch as a whole; its
reply has fence=0. This guarantee is not a rollback promise after hardware
submission. An admitted but partially executed record fails with `-EIO`
(or `-ETIMEDOUT` for a device completion deadline), a dirty indication and
an explicit recovery state. It never returns `-EOPNOTSUPP`. Never replay
an overlapping COPY, INVERT or partial cursor replacement on that path.

## Completion, fences and interleaving

Version 1 submission is synchronous: it executes the admitted records in
order, waiting for each record's completion proof before the next. There is
no asynchronous job queue or NONBLOCK submission flag. The batch returns a
completed count and fence id on success **or** a post-admission error. This
conservative choice preserves part A's synchronous hooks and gives precise
prefixes; a later async submission ABI would require another version.

The submission request returns the signed operation error (zero on success),
and delivers the 64-byte reply for every admitted batch, including errors.
On pre-admission refusal it delivers REFUSED with completed=0, admitted=0,
first_uncompleted=0, fence=0 and flags=0 if the reply was validated; an
invalid envelope/reply can fail without writing a reply. Callers must inspect
the reply on an admitted error, rather than treating a negative return as
permission to resubmit the whole batch. Fence controls return their own
transport/wait error; the snapshot's operation error remains the batch error.

One initial presenter acquisition admits the batch. The retained exclusive
drawing claim covers the entire batch, including all CPU tiles, native
submissions, intermediate SYNCs, completion waits and final drain. Set the
in-flight batch state before the first driver call; keep it set through the
final completion/stop proof. Both survive releases of the presenter mutex.
The existing 1 ms device-work budget remains: pause only at safe CPU tile
boundaries or bounded engine wait slices, release the mutex with IF=1,
yield, then reacquire and recheck cancellation/lifecycle state. A CPU tile
never spans such a yield with live user pointers or incomplete stores.

Other clients may stage, query cached state or request cancellation during
these releases. They cannot draw, submit engine commands, mutate cursor
state, run pixel probes/self-tests, remap/revoke targets or admit another
batch. Console retains text history and skips pixels; it never waits or
spins. System-memory reservations also survive releases. A mode/grant/lease
revocation requests cancellation and waits **outside** the presenter lock
for completion/stop proof before reclaiming references or mappings. Avoid
holding an object-table lock while waiting on this proof. A following batch
may interleave only after the previous batch is drained and its claim ends.
This is exclusive execution, not rollback atomicity or an atomic screen flip.

Let completed=k. Only records [0,k) have fully completed in order, including
CPU write drain and any required hardware wait; clipped no-ops count. Record
k is either unsubmitted or the one failed/uncertain record; no later record
has run. No automatic retry is permitted by a caller that ignores the reply.
For failure after submission, completed does not include the failed record,
even if it changed pixels. PARTIAL_DIRTY describes that record; the caller
must reconstruct every possibly modified destination of it after CPU safety
is proved. If no proof is obtained, no reconstruction or CPU access occurs.

Fence ids are nonzero u64 sequences scoped to the retained display-grant
object and owner process, not a pid alone. Allocate after preflight and
before execution; never reuse or wrap within that object's lifetime.
Allocate one fence per admitted batch, also for no-op/system-only batches.
Maintain at most 64 unreleased fences per grant; reserve a slot before any
drawing. Keep terminal records until explicit RELEASE or grant destruction.
The fence result survives loss of the user reply through process teardown.

Reply/status snapshot is 64 bytes:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0,4,6 | u32,u16,u16 | size=64, version=1, state |
| 8 | u64 | fence id, zero for pre-admission refusal |
| 16,20 | u32,u32 | completed count, first_uncompleted index (UINT32_MAX on success) |
| 24,28 | s32,u32 | operation error (0 on success), flags |
| 32,36 | u32,u32 | admitted record count, admitted display generation |
| 40 | u32[6] | zero |

State values: 0 REFUSED, 1 RUNNING, 2 SUCCEEDED, 3 STOPPED, 4 RECOVERING.
Flags: bit 0 PARTIAL_DIRTY, bit 1 CPU_SAFE, bit 2 CANCEL_REQUESTED; other
bits zero. CPU_SAFE permits access only to the retained targets covered by
this fence and does not validate old generations or restore invalid content.
RUNNING/RECOVERING are nonterminal; SUCCEEDED/STOPPED are terminal and
require no outstanding hardware access, drained CPU writes and CPU_SAFE.
An unsafe device failure stays RECOVERING until authoritative idle/stop
proof. It is not labeled terminal merely because submissions are quarantined.
An internal recovery owner retains the claim and all references if the caller
dies; cleanup cannot free an engine source or unmapped destination early.
The caller can receive a RECOVERING reply without releasing that ownership.

Fence control uses a 32-byte envelope: u32 size=32; u16 version=1 and
action; u64 fence at offset 8; u32 timeout_ms at 16; u32 reply_address at 20;
u32 reply_bytes=64 at 24; u32 reserved=0 at 28. The same grant, ownership,
range and no-fault reply rules apply. Actions 1 STATUS, 2 WAIT, 3 CANCEL,
4 RELEASE; timeout must be zero except WAIT (0–1000 ms). A fence can be
examined after a display mode change using its retained grant object, but
this does not authorize new drawing with a suspended or revoked grant.

- STATUS returns a coherent nonwaiting cached snapshot; it does not touch
  engine registers. Wrong owner is `-EACCES`, unknown/released id `-ENOENT`.
- WAIT polls when timeout=0, otherwise sleeps outside the presenter lock in
  bounded slices. It returns 0 when terminal, including STOPPED (read the
  operation error in the snapshot); `-ETIMEDOUT` while nonterminal at the
  deadline; `-EINTR` on a signal. Neither result cancels or frees the job.
  WAIT is not a CPU barrier unless the returned snapshot proves CPU_SAFE.
- CANCEL sets CANCEL_REQUESTED and requests stopping before the next
  unsubmitted record. An already submitted record finishes or enters
  recovery; no rollback and no false completion. It is idempotent, returning
  the current snapshot even for terminal fences. Cancellation is observed
  at record boundaries; a submitted software record finishes its tiles
  before stopping. In synchronous v1, public CANCEL is mainly useful for a
  returned RECOVERING fence; internal teardown uses the same stop request.
- RELEASE removes only a terminal fence; nonterminal is `-EBUSY`.
  Releasing terminal metadata does not invalidate target contents. A stale
  id cannot resolve to a new job. Grant close invokes cancellation/drain
  and frees records only when safe, never by abandoning unsafe hardware.

On success all records complete and fence SUCCEEDED is immediately visible.
On a clean runtime refusal at k, synchronize the prefix, set STOPPED with
completed=k and no PARTIAL_DIRTY. On a post-submission fault, stop issuing
records, quarantine submissions, preserve the prefix and prove idle/stop.
Return STOPPED with PARTIAL_DIRTY if proof succeeds; otherwise RECOVERING
without CPU_SAFE. Subsequent WAIT can prove safety, but never upgrades the
failed record to completed without proof of its exact output. A successful
fence is a completion/visibility boundary, not proof that another later
client has left the same screen pixels unchanged.

## Golden vectors for the implementation directive

These are expected results, not evidence that part C already executes.
Run each applicable vector at RGB565, BGR888 and XRGB8888, with padded and
unaligned rows and canaries. Raw native results are hexadecimal; symbolic
colours use P above. Untouched pixels and padding must remain byte-identical.
For byte expectations, emit low byte first using exactly 2/3/4 bytes.

| ID | Inputs | Expected pixels/result |
| --- | --- | --- |
| P1 | FILL RGB `123456`, 1×1 | `11aa` / `123456` / `00123456`. |
| P2 | FILL RGB `070307` then `080408` | 565: `0000`, then `0821`; 24/32 RGB: `070307`, then `080408`. Demonstrates truncation thresholds. |
| P3 | RGB555 16-bit target, FILL white | `7fff`, bytes `ff 7f`, reserved high bit zero. |
| P4 | 2×2 target, pitch=width*bytes+3, FILL at (-1,0), size 2×2, red | Only column 0 becomes `f800` / `ff0000` / `00ff0000`; column 1, padding and canaries unchanged. |
| C1 | One row `[R,G,B,W,K]`; COPY source x=0,w=4 to dx=1 | `[R,R,G,B,W]`. |
| C2 | Same; COPY source x=1,w=4 to dx=0 | `[G,B,W,K,K]`. |
| C3 | 2×3 `[R,G; B,W; K,R]`; COPY top two rows down one | `[R,G; R,G; B,W]`. |
| C4 | `[R,G,B,W,K]`; C1 geometry; clips (3,0,2,1) then (1,0,2,1), in this order | `[R,R,G,B,W]` as one snapshot; not sequential per-clip copies. |
| C5 | XRGB source `aa123456` to same-format target | Stored result `00123456`. |
| M1 | MONO width=10, bytes `81 80`, stride=2, red/blue | `[R,B,B,B,B,B,B,R,R,B]`; six low padding bits ignored. |
| M2 | M1, destination x=-7 on width=3 target, TRANSPARENT_BG, initial all G | `[R,R,G]`; destination column 0 samples source bit 7. |
| M3 | MONO 2×2, bytes `80 7e 40`, stride=2, white/black | `[W,K; K,W]`; byte `7e` is padding and ignored. |
| T1 | PATTERN bytes `80 40 20 10 08 04 02 01`, origin=(0,0), 3×2 at (0,0), white/black | `[W,K,K; K,W,K]`. |
| T2 | T1 pattern, origin=(-1,-1), destination (0,0) | W, since pattern coordinate is (1,1); clipping/splitting does not move origin. |
| I1 | INVERT RGB `123456` | `ee55` / `edcba9` / `00edcba9`. |
| I2 | XRGB raw `aa123456`, INVERT twice | First `aaedcba9`, then exact `aa123456`. RGB555 raw `9234` first becomes `edcb`, then `9234`. |
| I3 | Black row width=4; INVERT full row, clips (0,0,3,1), (1,0,3,1), (1,0,3,1) | `[W,W,W,W]` once, not XOR cancellation in columns 1–2. A second identical record restores black. |
| I4 | Nonzero clip count, all entries empty | No pixels; completed record. Contrast count=0, which means unrestricted. |
| K1 | KEYED source `[R,B,G]`, key B, target `[W,W,W]` | `[R,W,G]`. At 565, key `0000ff` also matches source RGB `0000f8` after quantization. |
| K2 | KEYED XRGB source `aa0000ff`, key `0000ff`, target raw `bb123456` | Target remains `bb123456`; source X is ignored. |
| S1 | NEAREST source `[K,W]` 2×1 to 3×1 | `[K,W,W]`; tie selects source index 1. |
| S2 | NEAREST source `[R,G,B]` 3×1 to 2×1 | `[R,B]`. |
| S3 | BILINEAR `[K,W]` 2×1 to 3×1 | RGB8 `[000000,808080,ffffff]`; 565 `[0000,8410,ffff]`. |
| S4 | 2×2 source `[K,R; G,B]`, BILINEAR to 1×1 | RGB `404040`, packed 565 `4208`; one final rounding of four channels. |
| S5 | S3, clip only destination x=1,w=1 | Only middle pixel becomes `8410` / `808080` / `00808080`; original scale transform preserved. |
| S6 | 565 source gray `8410`, SCALED 1×1 to 24/32 1×1 | RGB `848284` after channel expansion, not `808080`. |
| L1 | LINE (0,0)→(4,2) | `(0,0),(1,1),(2,1),(3,2)`; (4,2) excluded. |
| L2 | Reverse L1 | `(4,2),(3,2),(2,1),(1,1)`; (0,0) excluded. |
| L3 | LINE (0,0)→(2,4) | `(0,0),(1,1),(1,2),(2,3)`. |
| L4 | LINE (0,2)→(4,0) | `(0,2),(1,2),(2,1),(3,1)`; ties toward larger y. |
| L5 | LINE (-2,-1)→(2,1), clip x>=0,y>=0 | `(0,0),(1,1)`; no restart at the clip boundary. |
| L6 | LINE (1,1)→(1,1); LINE (2,3)→(2,0) | Empty; then `(2,3),(2,2),(2,1)`. |
| W1 | WRITE XRGB bytes `56 34 12 aa`, 1×1 | Same output as P1 at all depths, X ignored. |
| A1 | Cursor ARGB `80800000` over blue | RGB `80007f`; 565 `800f`. Input is premultiplied half-red. |
| A2 | Cursor ARGB `00000000` over raw XRGB `aa123456`; then `ffff0000` | Transparent leaves exact raw value; opaque red displays RGB `ff0000`. Backing pixels never change. |
| A3 | Cursor ARGB `80000080` over 565 `f800` | RGB8 `7f0080`, packed `7810`. |
| A4 | Cursor mono AND/XOR pairs 00,01,10,11 over blue; fg R,bg G | Displayed `[G,R,B,yellow]`; yellow `ffe0` / `ffff00` / `00ffff00`; backing unchanged. |
| E1 | SCALED or KEYED overlapping same allocation, even via another handle | `-EINVAL`, no pixel change, completed=0 on preflight, fence=0. |
| E2 | ARGB `8000ff00`, nonzero reserved field, unsupported opcode, bad extent | Respectively `-EINVAL`, `-EINVAL`, `-EOPNOTSUPP`, `-EINVAL` (or `-EOVERFLOW` if arithmetic overflows); no draw. |
| B1 | Admitted FILL R, INVERT, COPY; clean runtime refusal at COPY | completed=2, first_uncompleted=2, STOPPED, no PARTIAL_DIRTY; only prefix pixels present. |
| B2 | Same records, malformed COPY in blob | Entire preflight refusal: completed=0, fence=0, original pixels unchanged. |
| B3 | FILL complete, overlapping COPY submitted then device fault | completed=1; COPY excluded, suffix unsubmitted; PARTIAL_DIRTY; no software replay. STOPPED only with idle/stop proof, otherwise RECOVERING without CPU_SAFE. |
| B4 | Fake native MONO refuses before submission | Synchronize conflicting source/destination work, software produces M1 exactly once; native submitted count zero. |
| B5 | Pause fake driver during record 1, attempt another batch/console/mode revoke | Claim/in-flight/reservations remain; zero interleaved pixel/engine/cursor writes. Revoke waits outside mutex; references remain pinned. |
| B6 | WAIT on RECOVERING fence times out; CANCEL then RELEASE | WAIT `-ETIMEDOUT`, no ownership release; CANCEL sets request only; RELEASE `-EBUSY` until authoritative proof. |
| B7 | 65 records, 17 clips, 1,048,584 total bytes, exhausted 64 fence slots | `-E2BIG`, `-E2BIG`, `-E2BIG`, `-ENOSPC`, respectively; no draw and no allocated admitted fence. |
| B8 | Empty FILL then SYNC on admitted valid batch | completed=2, SUCCEEDED, CPU_SAFE, nonzero fence; no pixel change. |
| B9 | System-memory target with same geometry and inputs as P1–L6 | Same pixels, canaries and errors as CPU-mapped video target; revoked/unmapped engine-only target never becomes a system surface. |

R/G/B/W/K mean red/green/blue/white/black. Their 565 values are
`f800/07e0/001f/ffff/0000`; 24/32 values are
`ff0000/00ff00/0000ff/ffffff/000000` (zero X on stores).
Future host tests must also permute clip order, duplicate clip entries,
exercise both vertical COPY directions, negative origins, channel-swapped
layouts, all line octants and aliased handles. Driver tests must verify
refusal counters and completion proofs, rather than accepting software
fallback as evidence of native execution. Part C implementation, ABI
generation and its executable golden suite remain a separate directive.

## Lead decisions (2026-10-23 00:13)

The lead reviewed this specification; it is binding for f2-101 part C, with the four recommendations of the f2-101c report approved:
1. **Batches:** synchronous v1 batch submission and completion fences, with RECOVERING holding ownership until idle or stop is proved. An asynchronous queue is a later version.
2. **Pixels:** the exact pixel choices above, including truncation and expansion, reserved bits, colour-key comparison after packing, no KEYED or SCALED overlap, end-exclusive lines with explicit ties, centre sampling with bilinear rounding, premultiplied cursors and clip union.
3. **Admission limits:** a 1 MiB blob, 64 commands, 16 clips per command, 128 target references, 64 retained fences, and the dimension and extent caps. These are request limits, not memory reservations.
4. **Later numbering:** the part C code directive allocates the negotiated request and capability numbers and specifies the surface-creation extension for retained system-memory targets.

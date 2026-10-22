# Batch integration host checks

The binding scope is `docs/directives/integ-fix1-batch-integration-failures.md`.
The lead's flat-suite log is `build/lead-scratch/flat.log`. This record covers
host integration only; exact commands/results are in
`build/codex/integ-fix1-report.md`.

## Research and implementation decisions

- Microsoft's [update-region documentation](https://learn.microsoft.com/en-us/windows/win32/gdi/the-update-region)
  accumulates invalid portions before painting; [ScrollWindowEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-scrollwindowex)
  retains scrolled pixels and exposes an update region. The original
  [Windows 95 DDK guidelines](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S27F2.HTM)
  distinguish DIB-engine drawing and hardware cursor support. Keep the
  F2-98a/F2-98b clipped update discipline. `du_render` now invokes the paint
  hook for each region: Music's frame counter/pacing hook must count once
  per UI turn containing paint, while every region remains painted. Hardware
  cursor motion alone need not invalidate scene pixels in the orb replay.
- The [glibc Internet header](https://raw.githubusercontent.com/bminor/glibc/master/inet/netinet/in.h)
  declares IPPROTO_IP/TCP/UDP as enumerators. Drop the guest macros before
  including host networking headers in the existing replay adapter, matching
  its socket/signal macro policy. Leave the public ABI unchanged.
- The read-only Windows 98 Resource Kit, `wrkc10_6.htm` (VCACHE section) and
  `wrkc15_3.htm`/`wrkc15_7.htm` (TCP/IP and adapter configuration), was checked
  under `/home/peronslayer/Desktop/CiukiOS/build/rskt`. Preserve unified DOS
  native filesystem/cache ownership and the new separate network views.
  The owner-provider fixture must address UNIFIED.IMG and its digest sidecar,
  as F3-63 requires; it must still exercise real deletion and cleanup failure.
- [ACPICA's pinned release](https://github.com/open-acpica/acpica/releases/tag/20260930)
  and the retained per-file notices/manifest already have a matching About
  row and elected GPL-2.0 notice. Add the missing vendor-directory reference
  to that existing credits entry's documentation; do not duplicate the row.
- F2-99 explicitly authorizes the new original pointer silhouettes/shadow.
  Its [cursor validation record](f2-99-cursors.md) documents the upstream
  [Savage encoding](https://gitlab.freedesktop.org/xorg/driver/xf86-video-savage/-/blob/master/src/savage_cursor.c)
  and S3 manual. Cursor-only differences may be re-pinned after pixel and
  visual inspection. Changes outside the pointer or authorized credits/UI
  areas require investigation before recording a golden.

## Failure classification and visual evidence

The runtime regression is Music's per-region callback being counted as a
whole painted frame. The native hook now records only the first callback in
each UI turn. Its production-adapter test calls the hook twice in one turn
and again in the next; the publication replay still enforces its original
frame/publication bounds.

The other semantic conflicts are host fixtures: missing preemption/formatter
dependencies, a formatter linked twice, guest protocol macros preceding host
enumerators, direct text replacement without invalidation, and retired DOS
fixture names/source-text expectations. Network's eight original goldens
remain unchanged after explicit edit invalidation; every capture also checks
partial rendering against a full repaint. The synthetic DOS fixture stays
nonbootable parser data, checks real nested FAT read-back and verifies that
the retired classic payload is rejected. It needs no qualified guest image.

The DOS loading replay must return VIDEO/STATE events at its modeled 15-ms
producer period, rather than consuming the entire 50-ms maximum wait on
every event. TIMEOUT still consumes the full maximum. The unchanged startup
assertion observes 2,505 ms, excluding CREATE and real guest execution.
This follows the event-or-deadline distinction in Microsoft's
[wait-function contract](https://learn.microsoft.com/en-us/windows/win32/sync/wait-functions).
No production DOS cadence or ABI timeout constant was changed.

The orb replay now runs `desk_redraw`, including retained copies and exposed
regions, against its complete-render oracle. F2-98b permits a clipped copy
around shell chrome; the old blanket `!copy_pending` assertion was obsolete.
Its twenty original goldens remain unchanged. Pointer painting is excluded
from this scene-only fixture at each render boundary.

Historical-input diagnostic replays used the cursor array from `bcffa989^`
(with a null shadow pointer and its original dark colour). About additionally
used the credits data from `63965b9c`. Current production painters, input
paths and assertions were retained. No OS image or runtime asset was changed
for this comparison.

| Frame family | Historical pins reproduced | Authorized new pins |
| --- | --- | --- |
| CiukPaint | All 33 | Nine generic-pointer frames: crosshair, precision and I-beam; changed pixels only within their old/new footprints. Other 24 unchanged. F2-99. |
| Desktop reference | All 8 | Eight new arrow/shadow pins. Every pixel difference is inside x=510..531, y=382..411. F2-99. |
| Transient UI | All 12 | Only balloon, notification and tooltip include the new arrow/shadow. Other nine unchanged. F2-99. |
| Boot/memory fixture | All 9 | Nine new arrow/shadow pins. Allocation, cache, worker and performance assertions retained. F2-99. |
| About credits | All 4 | Four pins for the F2-99 cursor, F1-109 ACPICA and F2-123 PCI/USB ID rows, plus the F2-99 cursor credit. Identity header pixels remain identical. |

The new Paint cursor crops, desktop reference, About list and transient
captures were visually inspected. The detailed Ciuki portrait, its placement,
English tagline, typography and UI layout are preserved. No Network or orb
golden was changed.

Missing built SDK licences and the qualified UNIFIED.IMG/UNIFIED.json pair
remain environment prerequisites, not fixture substitutes or new skips.
LeakSanitizer thread inspection and execution of the 32-bit x87 test binary
are restricted by this sandbox. Exact remaining gates are distinguished from
assertion failures in the implementation report.

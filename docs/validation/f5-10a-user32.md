# F5-10A USER window/message core

Research performed before implementation, 2026-10-19:

- Microsoft Windows 98 Resource Kit, chapter 28, local extracted book
  `build/rskt/wrkc28_8.htm` (consulted in the main checkout): asynchronous
  input is routed to per-thread Win32 queues; Win16's shared queue is distinct.
  Chapter 25, `wrkc25_5.htm`: Win32 preemption and isolated address spaces.
- [PeekMessageA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-peekmessagea):
  sent, posted, hardware input, sent again, paint, timer; filters do not hide quit.
- [SendMessageA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagea):
  direct same-thread dispatch, receiver-thread execution and reentrant sends.
- [MsgWaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex):
  distinguish newly arrived input from existing unread input; atomic handle+queue wait.
- [BeginPaint](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-beginpaint):
  update clipping, validation and background erase.
- [SetTimer](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-settimer):
  minimum interval, replacement and DispatchMessage callback dispatch.
- [SetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setclipboarddata):
  clipboard ownership and movable global blocks.

The available online API references are current Microsoft documentation, not
an archived Windows 98 SDK. Modern UIPI, foreground locks, DPI and ghost-window
policy do not apply. Period SDK provenance remains a qualification limitation.

Repository comparison: f5-04 supplies fixed records and send reply tokens but
GET always removes, has no window/range filters, and waits use current input
rather than new input. f5-09 supplies the v3 parser/client bridge, surface
attachments and activation events. f5-07A supplies durable class atoms and HWNDs.
No Wine USER implementation is used; its spec is export metadata only.

Decisions: keep fixed call-96 record sizes and numbers; extend message actions
and wait flags additively, keeping legacy calls unchanged. Only the contract's
three copied pointer messages cross processes. Child windows share the root
surface. USER holds update regions and submits damaged rectangles after GDI
releases a clipped DC. The f5-10B interface and actual verification/limitations
are recorded in `build/codex/f5-10a-report.md`.

## Handoff and limits

The client binds the f5-10B `w32_gdi_surface_dc` interface observed in the
parallel worktree's `sdk/win32/include/w32/gdi32.h`, with a 1068-byte PE32
DIB descriptor, explicit XRGB8888 channel masks, client origin, a copied visible
region and a damage callback. USER's adapter uses `DeleteDC`, `CombineRgn`,
`GetRegionData`, `GetDIBits`, `CreateDIBSection` and `w32_gdi_draw_icon`.
The peer worktree was read only; no peer report or source was edited. This is
source-level interface alignment, not a cross-agent approval or merged-GDI
runtime qualification. The host fixture installs the same USER boundary with
a test raster sink; GDI's independent engine tests belong to f5-10B.

V3 operations that change window/pointer state wait for the matching result
request/window/generation. CREATE defers input/configuration callbacks until
WM_NCCREATE/WM_CREATE complete. Damage remains asynchronous, coalesced at 17 ms;
resize defers replacing the retained surface until all borrowed DCs are released.
The desktop's actual 3-pixel border and 24-pixel title geometry are used for
screen/client conversions; the window's child origin remains root-relative.

TEXT events are buffered against the originating key and emitted by
TranslateMessage; they do not manufacture characters from scan codes. TEXT
arriving after TranslateMessage completes the pending translation. Both layouts
reuse the existing desktop character tables, with explicit USER VK mappings.
The existing desktop configuration supplies `keyboard` and `double_click_ms`;
USER never imports the desktop theme. Focus loss and RESYNC discard buffered
translation and key state. Cross-thread v3 packets are delivered on their owning
UI thread and wake the kernel queue through same-process synthetic wake bits.

Class atoms and HWNDs use the service, including global window enumeration and
class-name queries. The clipboard extends the same fragment RPC with ephemeral
PID/TID ownership, copied format data, viewer/owner metadata and disconnect
cleanup; no clipboard bytes enter the durable hive or directory checkpoint.

Further primary references consulted during review:

- [InSendMessage](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-insendmessage),
  [DestroyWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-destroywindow).
  The directive explicitly asks children-first destruction; current Microsoft
  window lifecycle documentation describes parent WM_DESTROY before children.
  This implementation follows the written directive and records that divergence
  for the lead instead of silently changing the acceptance order.
- [Microsoft keyboard layout samples](https://github.com/microsoft/Windows-driver-samples/blob/main/input/layout/README.md):
  scan-to-VK and VK-to-character are distinct tables. No sample implementation
  or Wine USER source was copied.
- The [period Microsoft User Interface volume (2000)](https://www.bitsavers.org/pdf/microsoft/windows_2000/097-0002307_Microsoft_Windows_Volume_2_User_Interface_2000.pdf)
  was located, but complete retrieval failed (403/timeout). Its indexed excerpts
  are insufficient to claim a full period-SDK review. The local Windows 98 book
  and the cited Microsoft API documentation are the actual inspected sources.

The report records remaining feature limits and qualification work. Coverage
means each provisional census name has a real export or a typed W21 failure;
it does not mean every Windows 98 USER behavior, original acceptance import,
installer or application has been qualified.

The directive's cross-process WM_GETMINMAXINFO example conflicts with the
binding contract's explicit COPYDATA/SETTEXT/GETTEXT allowlist. The latter is
preserved here; MINMAX refusal is pinned in the production queue fixture. This
is an outstanding specification issue for the lead, not an expanded pointer
marshalling claim.

The final stub review checked non-Boolean failure conventions against
[WaitForInputIdle](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-waitforinputidle),
[GetMenuState](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenustate),
[ChangeDisplaySettingsA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changedisplaysettingsa)
and the [DDE error table](https://learn.microsoft.com/en-us/windows/win32/api/ddeml/nf-ddeml-ddegetlasterror).
Returning zero for these unimplemented operations would report success or a
valid menu state. The generated W21 bodies instead use -1 or DMLERR_SYS_ERROR
as appropriate, still setting error 120 and diagnosing once. A native adapter
executes the generated bodies, including repeated-call and diagnostic checks.

Italian OEM VK assignments were cross-checked as numeric ABI facts against the
[upstream Italian keyboard layout](https://github.com/reactos/reactos/blob/master/dll/keyboard/kbdit/kbdit.c).
No implementation was imported. The existing Ciuki character tables remain the
source for character conversion; `session.json` now pins all ten US/Italian OEM
positions as well as recorded KEY/TEXT traces. This upstream cross-check is not
a claim of an original Windows 98 keyboard-driver experiment.

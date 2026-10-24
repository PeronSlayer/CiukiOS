# fx-28 screenshot review contract

Research precedes the implementation; host evidence is not hardware evidence.

* [Microsoft CascadeWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-cascadewindows)
  and [TileWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-tilewindows)
  specify preserved stacking, restored maximised windows and excluded topmost/tool
  windows. These pages do not specify the geometry of non-sizing windows.
  The directive supplies the Windows 98 shell requirement: fixed-size windows
  retain their dimensions. Native arrangement already checks `resizable`, but
  Control Panel registers Display Properties as resizable. Register Display
  applet launches as fixed-size; retain the resizable Control Panel browser.
  Win32 arrangement must use SWP_NOSIZE for windows without WS_THICKFRAME.
  [Upstream Wine USER implementation](https://github.com/wine-mirror/wine/blob/master/dlls/user32/win.c)
  independently uses that flag for CascadeWindows without a sizing frame.
  The Microsoft API pages do not claim exact Windows 98 geometry; keep the
  directive's explicit fixed-size rule for both shell arrangements.
* [Microsoft IsHungAppWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-ishungappwindow)
  describes a five-second input-processing timeout and excludes input waits.
  It is a later API reference, not evidence of NT architecture in Windows 98.
  An unsent shell PING cannot measure client responsiveness. Start its deadline
  on successful transport delivery, not insertion into the desktop FIFO; shell
  menu and screenshot work before delivery must not consume the client's time.
  Keep genuine delivered-but-unanswered timeout and recovery coverage.
* [Microsoft's Windows 98 password instructions (preserved copy)](https://zx.net.nz/mirror/www.microsoft.com/windows98/usingwindows/work/articles/002Feb/password.html)
  specify Password protected and Change. The local Resource Kit,
  `wrkc09_4.htm`, makes this conditional on enabling saver protection.
  G32 is absent on this tree: show both controls disabled with a visible reason.
* [IBM Personal Computer User Guide, page 5-6](https://ps-2.kev009.com/pccbbs/aptiva/19k2413.pdf)
  documents Windows 98 Display > Screen Saver > Energy saving features of
  monitor > Settings opening Power Management. Launch `control --panel=power` through existing DESK_LAUNCH.
  DESK_SETTINGS_OPEN_POWER opens Shut Down and must not be used for this link.
  The saver tab has a monitor with animated miniature, a drop-down beginning
  with (None), Settings, full-screen Preview and an editable minutes spinner.
  Selection and minutes use the existing Apply/Cancel settings transaction.
* Drive General uses an elliptical disc with a shaded front edge, blue used
  and purple free sectors and matching legend swatches. Show grouped integer
  bytes and binary MB together for used/free/capacity, including empty/full
  discs and the existing unavailable/counting states. This visual requirement
  comes from the directive; the API references above do not define pie pixels.
* Keep the approved portrait and pinned OFL font sources. Extend the atlas
  with system punctuation and arrows, preserving existing glyph indices.
  [FreeType character mapping](https://freetype.org/freetype2/docs/reference/ft2-character_mapping.html#ft_get_char_index)
  defines missing character indices as zero. Reject missing additional atlas
  glyphs at conversion time. Inconsolata's absent non-breaking hyphen uses its
  ordinary hyphen ink; wrapping policy is outside the atlas.
  Both renderers must use one mapping. Audit C string literals, including
  escaped UTF-8, and verify actual glyph ink in UI/bold/caption faces.

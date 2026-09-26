# CiukiOS native desktop

A native, single-tasking graphical shell for the existing DOS-compatible
CiukiOS runtime. Boot opens the application library. DOS mode uses the same
interpreter, directory, history and launchers; EXIT or DESKTOP returns to the
GUI. Native DOS applications and Windows own the display while running.

Visual plan: slate desktop #28485C, midnight title #203448, light window
#CCD0D4, paper #F4F4EC, graphite #303C48, turquoise accent #58B8AC.
Liberation Sans 13px regular for controls and bold for window titles; ROM
monospace remains the DOS font. Deliberate signature: a stepped C mark and
three category tabs, rather than a Windows logo or a replicated Start menu.
The only presentation line is “A modern Retro OS”. All visible copy is English.

```
+----------------------------------------------------------------+
| CiukiOS                                       Programs  Run  DOS|
| [Programs]   +-- Applications ------------------ [_] [^] [x] -+ |
| [Files]      | Applications | Games | System                  | |
| [DOS]        |                                                | |
| [Display]    |   [Windows]      [Files]        [Editor]         | |
|              |   [Costa]        [DOS]          [Run]           | |
|              |                                                | |
|              +-- Double-click to open. Enter opens selection --+ |
| A modern Retro OS                                              |
| [CiukiOS] [Applications]                          [DOS] [clock] |
+----------------------------------------------------------------+
```

Review: retain period bevels, physical buttons and small type, but use the
slate palette, compact upper command strip and category library as the CiukiOS
identity. No promotional hero, decorative status claims or simulated windows.
Window controls, application launch, Run, power actions and DOS transition
must execute real behavior. Redraw must restore cursor pixels exactly.

Graphics follow the shared 640/800/1024 profile through checked, banked VBE.
Plain VGA remains a native 640x480x16 fallback. Mouse settings are privately
saved/restored around the desktop, and no shared interrupt handler is changed.

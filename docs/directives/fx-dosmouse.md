# fx-dosmouse: the mouse does not work in Warcraft II (and check every DOS game)

- **Written:** 2026-10-26 16:47 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: main be7852468.
- **Evidence (owner's T23 trial of image 5bb73dba, Pentium III 1 GHz, SuperSavage, 1024x768):** private logs in the lead's checkout: /home/peronslayer/Desktop/CiukiOS/legacy/local/physical/5bb73dba/serial.log (serial capture of several boots) and /home/peronslayer/Desktop/CiukiOS/legacy/local/physical/5bb73dba/disk/LOGS/BOOT*.LOG, CIUKIST.LOG (read them, never copy them into the tree; quote only the numbers you need). Earlier trial logs for comparison: legacy/local/physical/c42f556e/disk/LOGS and 2ca3b0b6.
- **Owner:** "the mouse does not work, for example in Warcraft II". Warcraft II demo runs under DOS/4GW Professional 1.97 with VESA modes, in a window and in full screen.

## Scope
1. **Find why.** Check every path:
   - INT 33h mouse services in V86 and through the DPMI host for protected-mode clients (DOS/4GW translates INT 33h, including AX=000Ch/0014h user handlers, which are called back in protected mode);
   - mouse range and mickey scaling in VESA modes (functions 07h/08h/0Fh/1Ah);
   - cursor visibility;
   - event delivery from the window (relative and absolute) and from full screen;
   - PS/2 versus USB input on the T23 (the Trackpoint is PS/2).

   Compare with the Windows 98 VMD (virtual mouse driver) behaviour and Microsoft MOUSE.COM semantics. Cite the sources: Ralf Brown INT 33h, DOS/4GW and DPMI 0.9 callbacks.
2. **Fix it** for Warcraft II in a window and in full screen, and verify DOOM, Wolfenstein 3-D, Quake and the DOS Box with a mouse-driven program.
3. **Tests.**
   - A host replay of Warcraft II's mouse initialisation and handler installation through the production DPMI/INT 33h path.
   - Event delivery in window and full-screen mode.
   - The existing DOS, DPMI and unified DOS suites stay green.

Report to build/codex/fx-dosmouse-report.md. Leave changes uncommitted.

## Lead amendment 1 (2026-10-26 23:54): review round 1

Your work is committed (f28d7e28c). The review (full text in build/codex/fx-dosmouse-review1.md) found two P1 and four P2. Fix all of them, each with a regression test:
1. P1: tests/host/test_f3_workloads.py:374-376 expects 'use_mouse 0'; update it to the new DOOM config and run the whole test file.
2. P1: in the DOS window, text 80x25 and 320x200 (mode 13h) are drawn at rows 40-439, so map clicks through the actual drawn rectangle. Fix the expected values encoded in dosmouse.asm:49 and dos_extenders.py:172.
3. P2: a DOS/4GW program that ends without removing its mouse handler (crash, Ctrl+C, extender error) must not fault the VM on the next movement. The same goes for a stuck callback followed by a reset: drop or revalidate the handler on client exit and on reset.
4. P2: bad INT 33h arguments must not stop the VM. Follow the Microsoft driver (swap min/max, clamp).
5. P2: reset (function 00h) must set the range from the current video mode (640x480, 50-line text and so on).
6. P2: implement functions 15h, 16h and 17h (state buffer size, save, restore) as the Microsoft driver does.
7. Lead decision on the DOOM mouse default (P3): keep use_mouse 1. In a window, mouse input reaches the game only after the user clicks inside the window to capture it, as in Windows 98's DOS box. Release the capture with Alt+Tab or Ctrl+Alt. Full screen always delivers. Add tests.

Also amend docs/design/f3-acceptance.md (fx-27) and docs/design/dos-dpmi-contract.md where they still say full screen keeps the ordinary exit; fx-doomfs2 is merged on main. Before you finish, merge main into this branch yourself (resolve dos_extenders.py by keeping both sides; Duke3D is on main). Report as "Review round 1". Leave changes uncommitted.

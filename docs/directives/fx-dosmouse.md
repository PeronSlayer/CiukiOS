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

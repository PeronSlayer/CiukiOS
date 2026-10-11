"""Start menu geometry of the desktop shell (apps/desktop/shell.c) at 1024x768, for the scripted QEMU cases.

The Start menu is 6 rows of 30 px plus one 8 px separator and 6 px of frame (194 px) above the 32 px
taskbar, so it spans y 542..736 and its first row, Programs, y 545..575 (x 2..186). Hovering or clicking
Programs opens its submenu at x 183..333, top-aligned with Programs, rows of 22 px from y 545, listing the
known programs present in /bin in PROGRAM_ORDER. A script clicks the Ciuki button, Programs, moves right
along the Programs row into the submenu, then down to the program row, clicks it and moves back."""
# SPDX-License-Identifier: MIT
PROGRAM_ORDER = ["ciuknote", "files", "tasks", "dosbox", "demo", "lua", "control"]
START_TOP = 768 - 32 - (6 + 6 * 30 + 8)
PROGRAMS_ROW = (START_TOP + 3, START_TOP + 3 + 30)
SUBMENU_X = (183, 333)


def program_row(programs, name):
    return [p for p in PROGRAM_ORDER if p in programs].index(name)


def check_launch(test, clicks, programs, name, label=""):
    """clicks[0] Ciuki button, clicks[1] Programs, clicks[2] the program's submenu row."""
    test.assertTrue(4 <= clicks[0][0] < 80 and 739 <= clicks[0][1] < 765, label)
    test.assertTrue(2 <= clicks[1][0] < 186 and PROGRAMS_ROW[0] <= clicks[1][1] < PROGRAMS_ROW[1], label)
    test.assertTrue(SUBMENU_X[0] <= clicks[2][0] < SUBMENU_X[1], label)
    test.assertEqual((clicks[2][1] - (START_TOP + 3)) // 22, program_row(programs, name), label)

"""Start menu geometry of the desktop shell (apps/desktop/shell.c) at 1024x768, for the scripted QEMU cases.

The Start menu is 6 rows of 30 px plus one 8 px separator and 6 px of frame (194 px) above the 32 px
taskbar, so it spans y 542..736 and its first row, Programs, y 545..575 (x 2..186). Hovering or clicking
Programs opens its submenu at x 183..333 with rows of 22 px: the known programs present in /bin in
PROGRAM_ORDER, then "Accessories" when an accessory is present, "Games" when dosbox is present (the bundled
shareware games, owner decision 2026-10-12), then a separator (8 px) and "Developer" when any other program
is present (shell.c build(), SH_MENU_PROGRAMS). The submenu is top-aligned with
Programs (y 542) unless it would cross the taskbar: then shell.c layout() moves it up to end at y 736.
A script clicks the Ciuki button, Programs, moves right along the Programs row into the submenu, then
up or down to the program row (submenu_row_y), clicks it and moves back.

Regenerating the cases after a submenu change: tests/host/desktop/shell_menu.py --regenerate rewrites the
move into the program row of each scripted case (and the following move, so every later pointer
position stays the same) from this model, then the move before each ANCHORS click so that click lands on
its absolute target (the later path moves with it)."""
# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import sys

PROGRAM_ORDER = ["ciuknote", "files", "tasks", "dosbox", "demo", "lua", "control"]
ACCESSORIES = ["calc", "charmap", "clock", "help", "mixer", "viewer"]
SCREEN_H = 768
TASKBAR_TOP = SCREEN_H - 32
START_TOP = TASKBAR_TOP - (6 + 6 * 30 + 8)
PROGRAMS_ROW = (START_TOP + 3, START_TOP + 3 + 30)
SUBMENU_X = (183, 333)
SUB_ROW = 22
SEPARATOR = 8
# Scripted cases and the program each one launches (tests/suites/f2-desktop.json).
TARGETS = {"desktop-shell-qemu-t23": "demo", "desktop-shell-qemu-e500": "demo", "files-qemu-t23": "files",
           "ciuknote-qemu-t23": "ciuknote", "tasks-qemu-t23": "tasks"}
# The first click after the launch (1-based click number, absolute target):
# Files runs its test at frame (4, 36), client (7, 60); its first scripted click is the View menu title,
# client (97, 10) in the Files UI harness (files_ui_test.c SCRIPT view_title), and the rest of the Files path
# is relative to it. Task Manager: its taskbar button (task buttons span y 739..765).
ANCHORS = {"files-qemu-t23": (4, (7 + 97, 60 + 10)), "tasks-qemu-t23": (4, (300, 751))}


def program_row(programs, name):
    return [p for p in PROGRAM_ORDER if p in programs].index(name)


def submenu_height(programs):
    rows = sum(p in programs for p in PROGRAM_ORDER)
    rows += any(p in programs for p in ACCESSORIES)
    rows += "dosbox" in programs                  # Games (shell.c SH_MENU_GAMES)
    developer = any(p not in PROGRAM_ORDER and p not in ACCESSORIES for p in programs)
    return 6 + (rows + developer) * SUB_ROW + (SEPARATOR if developer else 0)


def submenu_top(programs):
    """The submenu box's top edge: Programs' top minus the 3 px frame, moved up above the taskbar."""
    top = PROGRAMS_ROW[0] - 3
    return min(top, TASKBAR_TOP - submenu_height(programs))


def submenu_row_y(programs, name):
    """The centre line of the program's row in the Programs submenu."""
    return submenu_top(programs) + 3 + program_row(programs, name) * SUB_ROW + SUB_ROW // 2


def check_launch(test, clicks, programs, name, label=""):
    """clicks[0] Ciuki button, clicks[1] Programs, clicks[2] the program's submenu row."""
    test.assertTrue(4 <= clicks[0][0] < 80 and 739 <= clicks[0][1] < 765, label)
    test.assertTrue(2 <= clicks[1][0] < 186 and PROGRAMS_ROW[0] <= clicks[1][1] < PROGRAMS_ROW[1], label)
    test.assertTrue(SUBMENU_X[0] <= clicks[2][0] < SUBMENU_X[1], label)
    first = submenu_top(programs) + 3
    test.assertGreaterEqual(clicks[2][1], first, label)
    test.assertEqual((clicks[2][1] - first) // SUB_ROW, program_row(programs, name), label)


def image_programs(root):
    import re
    return sorted(set(re.findall(r'"/bin/([A-Za-z0-9_.-]+)"', (root / "scripts/build_image.py").read_text())) - {"desktop"})


def regenerate(root):
    """Moves each case's pointer onto its program row (submenu_row_y) and keeps every later position."""
    path = root / "tests/suites/f2-desktop.json"
    text = path.read_text()
    suite = json.loads(text)
    assert json.dumps(suite, indent=2) + "\n" == text, "f2-desktop.json is not in the canonical indent=2 form"
    programs = image_programs(root)
    changed = []
    for case in suite["cases"]:
        if case["id"] not in TARGETS:
            continue
        x, y, clicks, moves = 512, 384, 0, []
        for batch in case["actions"][0]["batches"]:
            for event in batch["events"]:
                data = event["data"]
                if event["type"] == "rel":
                    if data["axis"] == "x": x = min(max(x + data["value"], 0), 1023)
                    else:
                        before = y
                        y = min(max(y + data["value"], 0), SCREEN_H - 1)
                        moves.append((clicks, data, before))
                elif event["type"] == "btn" and data["down"]: clicks += 1
        into = [m for m in moves if m[0] == 2]
        after = [m for m in moves if m[0] == 3]
        assert len(into) == 1 and after, case["id"]
        _, data, before = into[0]
        target = submenu_row_y(programs, TARGETS[case["id"]])
        if target == before: target += 1          # never a zero-length move
        delta = target - (before + data["value"])
        if delta:
            data["value"] += delta
            after[0][1]["value"] -= delta
            changed.append((case["id"], delta))
        if case["id"] in ANCHORS:
            number, (ax, ay) = ANCHORS[case["id"]]
            x, y, clicks, last = 512, 384, 0, {}
            for batch in case["actions"][0]["batches"]:
                for event in batch["events"]:
                    data = event["data"]
                    if event["type"] == "rel":
                        before = x if data["axis"] == "x" else y
                        value = min(max(before + data["value"], 0), (1023 if data["axis"] == "x" else SCREEN_H - 1))
                        if data["axis"] == "x": x = value
                        else: y = value
                        last[data["axis"]] = (data, before)
                    elif event["type"] == "btn" and data["down"]:
                        clicks += 1
                        if clicks == number:
                            for axis, target, now in (("x", ax, x), ("y", ay, y)):
                                if target != now:
                                    move, before = last[axis]
                                    move["value"] = target - before
                                    changed.append((case["id"], f"click {number} {axis} {now}->{target}"))
                            break
                else:
                    continue
                break
    path.write_text(json.dumps(suite, indent=2) + "\n")
    return changed


if __name__ == "__main__" and sys.argv[1:] == ["--regenerate"]:
    print(regenerate(Path(__file__).resolve().parents[3]))

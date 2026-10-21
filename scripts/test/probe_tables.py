"""The three probe-name tables a suite selector must be present in (f5-17B).

A suite case selects a guest probe with `f<phase>:<name> run=<id>`. Three
independent sources decide whether that name is selectable:

* the kernel selector, `src/kernel/probes/selector.c` (`f<phase>_names`), which
  is the only table the QEMU fw_cfg path consults;
* the boot-loader menu, `src/boot/ciukldr/menu.inc` (`f<phase>_probe_names`),
  which a person types on a physical machine;
* the host reference model, `scripts/test/loader_model.py`
  (`F<phase>_PROBES` and its selector grammar).

This module only reads them; nothing here registers or invents a probe. The
f5-17B host test (tests/host/test_runner_selectors.py) compares every suite
case against all three, and `win32_runner` refuses (not_run, never pass) a
Win32 title case whose probe the kernel table does not know yet.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import loader_model  # noqa: E402

SELECTOR_C = 'src/kernel/probes/selector.c'
MENU_INC = 'src/boot/ciukldr/menu.inc'
PHASES = range(7)
SELECTOR_PREFIX = re.compile(r'f([0-6]):([a-z][a-z0-9]*(?:-[a-z0-9]+)*)(?: |$)')
SOURCES = ('kernel', 'menu', 'loader_model')
# struct probe_selection.probe[24] (src/kernel/probes/selector.h): the kernel
# selector keeps at most 23 bytes of a probe name.
PROBE_LIMIT = 23


def split_selector(selector):
    """(phase, probe name) of a suite `selector` string, or None for a case
    that is not selected by name (a normal boot has no selector at all)."""
    if not isinstance(selector, str):
        return None
    match = SELECTOR_PREFIX.match(selector)
    return (int(match[1]), match[2]) if match else None


def kernel_names(root=ROOT):
    """phase -> names of the kernel selector's `f<phase>_names[]` arrays."""
    text = (Path(root) / SELECTOR_C).read_text()
    tables = {}
    for match in re.finditer(r'static const char \*const f([0-6])_names\[\]\s*=\s*\{(.*?)\};', text, re.S):
        tables[int(match[1])] = set(re.findall(r'"([^"\\]+)"', match[2]))
    return tables


def menu_names(root=ROOT):
    """phase -> names of the loader menu's NUL-separated `db` lists.

    The F0 list is `probe_names:`; F1..F6 are `f<n>_probe_names`. A list ends
    at the first `db` line that finishes with the double NUL terminator.
    A labelled `db` line inside a list (`f2_crash_name db`, f3-55's
    `dos_conformance_name db`, `f3_title_name db`) is still part of it, as
    are comments and blank lines; any other unindented line (a code label or
    directive) ends a list that lacks its terminator.
    """
    tables, current = {}, None
    for line in (Path(root) / MENU_INC).read_text().splitlines():
        label = re.match(r'(probe_names|f([1-6])_probe_names)\b', line)
        if label:
            current = 0 if label[1] == 'probe_names' else int(label[2])
            tables[current] = set()
        elif current is None:
            continue
        elif line[:1] not in (' ', '\t', ';', '') and not re.match(r'[A-Za-z_]\w*:?\s+db\b', line):
            current = None  # a code label or other directive ends the list
            continue
        if current is None:
            continue
        code = line.split(';', 1)[0]
        if re.search(r'\bdb\b', code):
            tables[current].update(re.findall(r"'([^']+)'", code))
            if code.rstrip().endswith(',0,0'):
                current = None
    return tables


def loader_model_accepts(phase, name):
    """True when the host reference model's selector grammar accepts the name."""
    try:
        request = 'f%d:%s run=00000000' % (phase, name)
        if phase == 3 and name == 'title': request += ' title=fixture'
        if phase == 3 and name == 'dos-conformance': request += ' guest=freedos'
        if phase == 6 and name == 'browser': request += ' url=https://fixture.test/page'  # f6-18: the URL is mandatory
        loader_model.selector(request)
    except ValueError:
        return False
    return True


def registration(phase, name, root=ROOT):
    """Which sources know `f<phase>:<name>`: a dict of booleans per source."""
    return {'kernel': name in kernel_names(root).get(phase, ()),
            'menu': name in menu_names(root).get(phase, ()),
            'loader_model': loader_model_accepts(phase, name)}


def missing(phase, name, root=ROOT):
    """Sources (SOURCES order) that do not know the probe name."""
    known = registration(phase, name, root)
    return [source for source in SOURCES if not known[source]]

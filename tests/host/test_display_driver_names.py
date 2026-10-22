"""DM12: forbid graphics-family policy/calls in generic source, not substrings."""
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[2]
FAMILY=r'(?:s3savage|savage(?:2d)?|supersavage|mach64|rage|radeon|nvidia|riva|matrox|mga|i810|i830|voodoo|tdfx|trident|sis|neomagic|via)'
NAME=re.compile(r'\b'+FAMILY+r'(?:_[a-z0-9_]+)?\b',re.I)
# Frozen driver interfaces are private compatibility declarations until part B.
PRIVATE_HEADERS={'src/kernel/include/ciuki/savage2d.h',
    'src/kernel/include/ciuki/supersavage_display.h','src/kernel/include/ciuki/supersavage_mode.h'}
# Audio identity is outside the graphics-driver policy. These exact lines are
# allowed, not all occurrences of a chip substring or an entire generic file.
EXACT={
    'src/kernel/drivers/video/registry.c': {
        'extern const struct display_driver s3savage_driver; /* declarative registration */',
        'static const struct display_driver *const drivers[] = { &s3savage_driver };'},
    'src/kernel/include/ciuki/ac97.h': {'bool sis, nvidia, sis_gate_changed;'},
    'src/kernel/core/devices.c': {'{"ac97", "Intel ICH, SiS 7012 and nForce AC97; safe mode disables activation"},'},
}
VENDORED=('sdk/win32/lz32/mspack/',)

def violations(path,source):
    if path in PRIVATE_HEADERS or path.startswith(VENDORED): return []
    # Comments are not policy or calls. Retain newlines for actionable locations.
    source=re.sub(r'/\*.*?\*/|//[^\n]*',lambda m:'\n'*m[0].count('\n'),source,flags=re.S)
    result=[]
    original=(ROOT/path).read_text().splitlines() if (ROOT/path).exists() else source.splitlines()
    for number,line in enumerate(source.splitlines(),1):
        if NAME.search(line) and original[number-1].strip() not in EXACT.get(path,set()):
            result.append((path,number,line.strip()))
    return result

class NamesTests(unittest.TestCase):
    def test_generic_sources(self):
        paths=[ROOT/'src/kernel/drivers/fbdev.c',ROOT/'src/kernel/drivers/video/registry.c']
        for folder in ('src/kernel/core','src/kernel/include/ciuki','apps','sdk'):
            paths.extend(p for p in (ROOT/folder).rglob('*') if p.suffix in ('.c','.h'))
        bad=[]
        for path in sorted(set(paths)):
            bad.extend(violations(path.relative_to(ROOT).as_posix(),path.read_text()))
        self.assertEqual(bad,[],'\n'.join(f'{p}:{n}: {s}' for p,n,s in bad))

    def test_policy_examples(self):
        self.assertTrue(violations('example.c','savage2d_init(fb,0);'))
        self.assertTrue(violations('example.c','if (!strcmp(id,"radeon")) activate();'))
        self.assertFalse(violations('example.c','static int private_data; /* via a private map */'))
        self.assertTrue(violations('example.c','void nvidia_fill(void);'))

if __name__=='__main__': unittest.main()

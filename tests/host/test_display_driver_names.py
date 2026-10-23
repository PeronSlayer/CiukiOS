"""DM12: forbid graphics-family policy/calls in generic source, not substrings."""
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[2]
FAMILY=r'(?:s3trio64|s3virge|trio64|virge|s3savage|savage(?:2d)?|supersavage|chipshiqv|mach64|rage(?:128)?|radeon(?:2d)?|nv2d|nvidia|riva(?:128)?|nv3|matrox|mga|i740|i810|i830|intel2d|voodoo|tdfx|trident|sis(?:300|315)?|neomagic|via|unichrome|cirrus|gd54\w*|vmwaresvga)'
NAME=re.compile(r'\b'+FAMILY+r'(?:_[a-z0-9_]+)?\b',re.I)
# Native implementation and private headers are confined to the driver tree.
PRIVATE_DRIVERS=(
    'src/kernel/drivers/video/chipshiqv/',
    'src/kernel/drivers/video/cirrus/',
    'src/kernel/drivers/video/i740/',
    'src/kernel/drivers/video/i810/',
    'src/kernel/drivers/video/intel2d/',
    'src/kernel/drivers/video/mach64/',
    'src/kernel/drivers/video/matrox/',
    'src/kernel/drivers/video/neomagic/',
    'src/kernel/drivers/video/nv2d/',
    'src/kernel/drivers/video/radeon2d/',
    'src/kernel/drivers/video/rage128/',
    'src/kernel/drivers/video/riva128/',
    'src/kernel/drivers/video/s3legacy/',
    'src/kernel/drivers/video/s3savage/',
    'src/kernel/drivers/video/s3trio64/',
    'src/kernel/drivers/video/s3virge/',
    'src/kernel/drivers/video/sis/',
    'src/kernel/drivers/video/tdfx/',
    'src/kernel/drivers/video/trident/',
    'src/kernel/drivers/video/unichrome/',
    'src/kernel/drivers/video/vmwaresvga/',
)
PRIVATE_DRIVER='src/kernel/drivers/video/s3savage/'
# Audio identity is outside the graphics-driver policy. These exact lines are
# allowed, not all occurrences of a chip substring or an entire generic file.
EXACT={
    'src/kernel/drivers/video/registry.c': {
        'extern const struct display_driver mach64_driver; /* declarative registration */',
        'extern const struct display_driver rage128_driver; /* declarative registration */',
        'extern const struct display_driver radeon2d_driver; /* declarative registration */',
        'extern const struct display_driver sis300_driver; /* declarative registration */',
        'extern const struct display_driver sis315_driver; /* declarative registration */',
        'extern const struct display_driver cirrus_driver; /* declarative registration */',
        'extern const struct display_driver trident_blade_driver; /* declarative registration */',
        'extern const struct display_driver chipshiqv_driver; /* declarative registration */',
        'extern const struct display_driver matrox_driver; /* declarative registration */',
        'extern const struct display_driver nv2d_driver; /* declarative registration */',
        'extern const struct display_driver riva128_driver; /* declarative registration */',
        'extern const struct display_driver neomagic_driver; /* declarative registration */',
        'extern const struct display_driver unichrome_driver; /* declarative registration */',
        'extern const struct display_driver tdfx_driver; /* declarative registration */',
        'extern const struct display_driver vmwaresvga_driver; /* declarative registration */',
        'extern const struct display_driver s3savage_driver; /* declarative registration */',
        'extern const struct display_driver s3trio64_driver; /* declarative registration */',
        'extern const struct display_driver s3virge_driver; /* declarative registration */',
        'extern const struct display_driver i740_driver; /* declarative registration */',
        'extern const struct display_driver i810_driver; /* declarative registration */',
        'extern const struct display_driver intel2d_gen2_driver; /* declarative registration */',
        'extern const struct display_driver intel2d_gen3_driver; /* declarative registration */',
        '&mach64_driver,',
        '&rage128_driver,',
        '&radeon2d_driver,',
        '&sis300_driver,',
        '&sis315_driver,',
        '&cirrus_driver,',
        '&trident_blade_driver,',
        '&chipshiqv_driver,',
        '&matrox_driver,',
        '&nv2d_driver,',
        '&riva128_driver,',
        '&neomagic_driver,',
        '&unichrome_driver,',
        '&tdfx_driver,',
        '&vmwaresvga_driver,',
        '&s3savage_driver,',
        '&s3trio64_driver,',
        '&s3virge_driver,',
        '&i740_driver,',
        '&i810_driver,',
        '&intel2d_gen2_driver,',
        '&intel2d_gen3_driver,',
    },
    'src/kernel/include/ciuki/ac97.h': {'bool sis, nvidia, sis_gate_changed;',
        'bool observed_halt, codec_changed, software_master, beep_present, cirrus_spdif;'},
    'src/kernel/core/devices.c': {'{"ac97", "Intel ICH, SiS 7012 and nForce AC97; safe mode disables activation"},'},
}
VENDORED=('sdk/win32/lz32/mspack/',)

def violations(path,source):
    if path.startswith(PRIVATE_DRIVERS) or path.startswith(VENDORED): return []
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
        for folder in ('src/kernel/core','src/kernel/drivers/video','src/kernel/include/ciuki','apps','sdk'):
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
        self.assertTrue(violations('example.c','riva128_start(fb,0);'))
        self.assertTrue(violations('example.c','nv3_engine_init(engine,io,fb,ram);'))
        self.assertTrue(violations('src/kernel/include/ciuki/savage2d.h','void savage2d_init(void);'))
        self.assertTrue(violations('src/kernel/drivers/video/another/adapter.c','void savage2d_init(void);'))
        self.assertFalse(violations(PRIVATE_DRIVER+'savage2d.h','void savage2d_init(void);'))

        self.assertTrue(violations('example.c','radeon2d_driver.start(fb,0);'))
        self.assertTrue(violations('example.c','cirrus_start(fb);'))

if __name__=='__main__': unittest.main()

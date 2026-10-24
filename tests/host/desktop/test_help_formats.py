"""G15B format, navigation and question-driven Help with ASan/UBSan/LSan."""
# SPDX-License-Identifier: MIT
import importlib.util
import os
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path); result=importlib.util.module_from_spec(spec); spec.loader.exec_module(result); return result

class HelpFormatTests(unittest.TestCase):
    def test_htmlhelp_forwarder(self):
        from scripts.win32 import help as htmlhelp, census, stubs, upper_dlls
        parent=ROOT/'build/host'; parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='htmlhelp-pe-',dir=parent) as tmp:
            record=htmlhelp.build(Path(tmp),0x70900000)
            pe=census.PE((Path(tmp)/record['path']).read_bytes()).census()
            exports={name:e['forwarder'] for e in pe['exports'] for name in e['names']}
            self.assertEqual(exports,{'HtmlHelpA':'user32.CiukiHtmlHelpA','HtmlHelpW':'user32.CiukiHtmlHelpW'})
            self.assertEqual({n:e['ordinal'] for e in pe['exports'] for n in e['names']},{'HtmlHelpA':14,'HtmlHelpW':15})
            self.assertEqual(record['version'],[0,0,0,0])
            manifest=Path(tmp)/'builtin-store.json'; manifest.write_text(json.dumps({'schema':1,'dlls':[record]}))
            self.assertEqual(stubs.verify_store(manifest),1)
            self.assertEqual(set(upper_dlls.read_images(tmp)),{'hhctrl.ocx'})
            self.assertEqual(stubs.imports('STATIC hhctrl.ocx -> HtmlHelpA, HtmlHelpW'),{'hhctrl.ocx':{'HtmlHelpA','HtmlHelpW'}})
            print('PASS G15B hhctrl.ocx A/W PE forwarders and version resource')

    def test_formats_and_navigation(self):
        fixtures=module('help_fixture_data',HERE/'help_fixtures.py')
        accessories=module('help_format_accessories',HERE/'test_accessories.py')
        icons=module('help_format_icons',HERE/'icon_assets.py')
        parent=ROOT/'build/host'; parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='help-formats-',dir=parent) as tmp:
            out=Path(tmp); (out/'home').mkdir()
            for name,data in {'phrases.hlp':fixtures.hlp(), 'phrases40.hlp':fixtures.hlp(phrases40=True),
                              'compressed.hlp':fixtures.hlp(topic_compressed=True), 'stored.chm':fixtures.chm(),
                              'lzx.chm':fixtures.chm(compressed=True)}.items(): (out/name).write_bytes(data)
            flags=[f for f in accessories.FLAGS if f!='-Wl,--gc-sections']
            sources=[*accessories.TESTS['help'],*accessories.TOOLKIT,'apps/accessories/acc.c','tests/host/desktop/help_formats_test.c']
            binary=out/'help-formats'
            subprocess.run([*flags,'-Wl,--gc-sections',*accessories.INCLUDES,*icons.defines(),
                            '-I',str(ROOT/'sdk/win32/lz32/mspack'),'-DHAVE_INTTYPES_H=1','-DSIZEOF_OFF_T=8',
                            *(str(ROOT/s) for s in sources),'-lm','-o',str(binary)],check=True)
            env={**os.environ,'HOME':str(out/'home'),'TMPDIR':str(out)}
            # Preserve repository sanitizer settings; no leak-detection override.
            result=subprocess.run([str(binary),tmp],text=True,capture_output=True,env=env,timeout=120)
            print(result.stdout,end='')
            with self.subTest(authored_formats=True): self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            if os.environ.get('CIUKI_HELP_OWNER_MEDIA'):
                media=Path(os.environ['CIUKI_HELP_OWNER_MEDIA'])
                for name in ('HELP/RK98BOOK.CHM','HELP/WIN98RK.CHM','CONFIG/TZEDIT.HLP','FILE/WINDIFF.HLP','DIAGNOSE/DEPENDS.HLP'):
                    with self.subTest(owner_local=name):
                        result=subprocess.run([str(binary),'--real',str(media/name)],text=True,capture_output=True,env=env,timeout=120)
                        print(name+': '+result.stdout,end='')
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__': unittest.main()

"""Desktop folder host input replay, ASan/UBSan; no QEMU or image build."""
# SPDX-License-Identifier: MIT
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture
from .test_window_resize import png
from . import icon_assets

ROOT = Path(__file__).resolve().parents[3]

class DesktopFolderTests(unittest.TestCase):
    def test_folder_shell_replay(self):
        parent=ROOT / 'build/host'
        parent.mkdir(parents=True,exist_ok=True)
        review=ROOT / 'build/f2-42'
        review.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='desktop-folder-',dir=parent) as tmp:
            folder=Path(tmp)
            env=dict(os.environ,TMPDIR=tmp,ASAN_OPTIONS='detect_leaks=0')
            common=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                    '-include',str(ROOT / 'tests/host/desktop/replay_base.h'),'-D_POSIX_C_SOURCE=200809L','-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
                    *icon_assets.defines(),'-I',str(ROOT / 'apps/desktop'),'-I',str(ROOT / 'src/kernel/include')]
            sources=['tests/host/desktop/desktop_folder_test.c','tests/host/desktop/fake.c','tests/host/desktop/fake_vfs.c',
                     *('apps/desktop/'+n+'.c' for n in ('client','protocol','input','compositor','theme','shell','autoplay','autoplay_content','settings','wallpaper','keymap','folder')),
                     'apps/files/model.c','apps/files/transfer.c','apps/libdeskui/draw.c','apps/libdeskui/icons.c','apps/libdeskui/icon_map.c']
            objects=[]
            for i,source in enumerate(sources):
                target=folder / f'{i}.o'
                subprocess.run([*common,*(['-Dfake_reset=fake_tree_reset'] if source.endswith('fake_vfs.c') else []),
                                '-c',str(ROOT / source),'-o',str(target)],check=True,env=env)
                objects.append(str(target))
            binary=folder / 'folder-test'
            subprocess.run(['clang','-fsanitize=address,undefined',*objects,'-o',str(binary)],check=True,env=env)
            spec=importlib.util.spec_from_file_location('folder_portrait',ROOT / 'apps/desktop/convert_portrait.py')
            converter=importlib.util.module_from_spec(spec);spec.loader.exec_module(converter)
            portrait=folder / 'portrait.xrgb';portrait.write_bytes(converter.convert(ROOT / 'assets/brand/ciuki-logo.png'))
            frames=folder / 'frames';frames.mkdir()
            result=subprocess.run([str(binary),str(frames),str(portrait)],capture_output=True,text=True,env=env)
            self.assertEqual(result.returncode,0,result.stdout+'\n'+result.stderr)
            print(result.stdout.strip())
            for raw in frames.glob('*.xrgb'):
                capture(raw.stem, raw.read_bytes(), 1024, 768)
                (review / (raw.stem+'.png')).write_bytes(png(raw.read_bytes(),1024,768))
            self.assertEqual(len(list(frames.glob('*.xrgb'))),7)

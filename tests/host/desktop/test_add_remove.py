"""f2-87 fixture inventory and production FAT removal; no canonical image."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[3]
class AddRemoveTests(unittest.TestCase):
    def test_affected_kernel_compile(self):
        # Read just the literal target flags; never import or run the builder.
        import ast
        tree=ast.parse((ROOT/'scripts/build_kernel.py').read_text())
        constants={}
        for node in tree.body:
            if isinstance(node,ast.Assign) and len(node.targets)==1 and isinstance(node.targets[0],ast.Name):
                try:constants[node.targets[0].id]=ast.literal_eval(node.value)
                except (ValueError,TypeError):pass
        flags_node=next(node.value for node in tree.body if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='CFLAGS' for t in node.targets))
        flags=[]
        for node in flags_node.elts:
            if isinstance(node,ast.JoinedStr):
                flags.append(''.join(p.value if isinstance(p,ast.Constant) else str(constants[p.value.id]) for p in node.values))
            else:flags.append(ast.literal_eval(node))
        work=ROOT/'build/host/f2-87';work.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            for source in ('fs/program_tree.c','vm/dosvm_programs.c','vm/dosvm_client.c','vm/dosvm_disk.c'):
                subprocess.run(['clang',*flags,'-I'+str(ROOT/'src/kernel/include'),'-c',ROOT/'src/kernel'/source,
                                '-o',Path(temporary)/(Path(source).stem+'.o')],check=True)

    def test_inventory_and_removals(self):
        work=ROOT/'build/host/f2-87';work.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            folder=Path(temporary)
            for name in ('Games/Test/Sub','Programs/UserDirectory','Programs/DosUser','Programs/Win','Windows/Desktop','Windows/Start Menu/Programs','Documents/Desktop','system/licenses','system/settings'):
                (folder/name).mkdir(parents=True,exist_ok=True)
            (folder/'Games/Test/Sub/data').write_bytes(b'fixture')
            (folder/'Programs/UserDirectory/file').write_bytes(b'fixture')
            (folder/'system/settings/dos-game-3.cfg').write_text('kernel=0\n')
            for package in ('wolf3d-shareware','doom-shareware','quake-shareware'):
                shutil.copyfile(ROOT/f'third_party/games/{package}/UPSTREAM.json',folder/f'system/licenses/{package}-UPSTREAM.json')
            env=dict(os.environ,TMPDIR=temporary)
            command=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L',
                     '-include',str(ROOT/'tests/host/desktop/replay_base.h'),'-fsanitize=address,undefined',
                     '-fno-sanitize-recover=undefined','-I'+str(ROOT/'src/kernel/include'),
                     str(ROOT/'apps/control/programs.c'),str(ROOT/'apps/desktop/protocol.c'),
                     str(ROOT/'tests/host/desktop/programs_test.c'),'-o',str(folder/'programs-test')]
            subprocess.run(command,check=True,env=env)
            subprocess.run([folder/'programs-test',folder],check=True,env=env)

    def test_fixture_fat_removal(self):
        work=ROOT/'build/host/f2-87';work.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            folder=Path(temporary);image=folder/'fixture.img'
            with image.open('wb') as stream:stream.truncate(16*1024*1024)
            env=dict(os.environ,TMPDIR=temporary,MTOOLS_SKIP_CHECK='1')
            subprocess.run(['mkfs.fat','--invariant','-F','16','-s','2',image],check=True,env=env,capture_output=True)
            for name in ('WOLF3D','DOOM','DOOM/Subdirectory','QUAKE'):
                subprocess.run(['mmd','-i',image,'::/'+name],check=True,env=env)
            payload=folder/'data';payload.write_bytes(b'fixture data'*300)
            for name in ('WOLF3D/KEEP.BIN','DOOM/Subdirectory/Long file name.bin','DOOM/DOOM.EXE','QUAKE/KEEP.BIN'):
                subprocess.run(['mcopy','-i',image,payload,'::/'+name],check=True,env=env)
            sources=['src/kernel/fs/'+name+'.c' for name in ('fs_port','blkreq','cache','path','fat','program_tree')]
            sources+=['tests/host/fs/fake.c','src/kernel/lib/sha256.c','tests/host/desktop/program_tree_test.c']
            binary=folder/'fat-test'
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L',
                            '-DFS_HOST','-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
                            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                            '-I'+str(ROOT/'src/kernel/include'),'-I'+str(ROOT/'src/kernel/fs'),
                            *(str(ROOT/s) for s in sources),'-pthread','-o',binary],check=True,env=env)
            ran=subprocess.run([binary,image],env=env)
            subprocess.run(['fsck.fat','-n',image],check=True,env=env)
            listing=subprocess.check_output(['mdir','-b','-s','-i',image,'::/'],env=env,text=True)
            self.assertNotIn('DOOM',listing);self.assertIn('WOLF3D/KEEP.BIN',listing);self.assertIn('QUAKE/KEEP.BIN',listing)
            self.assertEqual(ran.returncode,0,'FAT assertions/fsck completed; sanitizer must also finish successfully')
if __name__=='__main__':unittest.main()

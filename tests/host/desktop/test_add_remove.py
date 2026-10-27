"""f2-87 fixture inventory and production FAT removal; no canonical image."""
from pathlib import Path
import hashlib
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
            for source in ('fs/program_tree.c','vm/dosvm_programs.c','vm/dosvm_client.c','vm/dosvm_disk.c','vm/dosvm.c','proc/supervisor.c'):
                subprocess.run(['clang',*flags,'-I'+str(ROOT/'src/kernel/include'),'-c',ROOT/'src/kernel'/source,
                                '-o',Path(temporary)/(Path(source).stem+'.o')],check=True)

    def test_inventory_and_removals(self):
        work=ROOT/'build/host/f2-87';work.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            folder=Path(temporary)
            for name in ('Games/Test/Sub','Programs/UserDirectory','Programs/DosUser','Programs/Win','Windows/Desktop','Windows/Start Menu/Programs','Documents/Desktop','system/licenses','system/settings','System'):
                (folder/name).mkdir(parents=True,exist_ok=True)
            (folder/'Games/Test/Sub/data').write_bytes(b'fixture')
            (folder/'Programs/UserDirectory/file').write_bytes(b'fixture')
            (folder/'system/settings/dos-game-3.cfg').write_text('kernel=0\n')
            for package in ('wolf3d-shareware','doom-shareware','quake-shareware','war2-demo','duke3d-shareware'):
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
    def test_supervisor_management_delegation(self):
        work=ROOT/'build/host/f2-87';work.mkdir(parents=True,exist_ok=True)
        source=(ROOT/'src/kernel/proc/supervisor.c').read_text()
        start=source.index('void supervisor_dosvm_delegate(')
        body=source[start:source.index('\n}',start)+2]
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            folder=Path(temporary);harness=folder/'delegation.c';binary=folder/'delegation'
            harness.write_text(r'''#include <ciuki/kernel.h>
#include <ciuki/desktop.h>
#include <assert.h>
static struct process supervisor,parent,child;
static struct desktop_description grants[2];
struct process *proc_supervisor(void){return &supervisor;}
struct desktop_description *desktop_fd(const struct process *p,int32_t fd)
{return p==&parent && fd>=0 && fd<2 ? &grants[fd] : 0;}
'''+body+r'''
int main(void){
    parent.pid=42;grants[0].kind=DESKTOP_DISPLAY;grants[1].kind=DESKTOP_INPUT;
    grants[0].owner=grants[1].owner=42;
    supervisor_dosvm_delegate(&supervisor,&child,"/Programs/desktop");assert(child.programs_capability && !child.dosvm_capability);
    supervisor_dosvm_delegate(&parent,&child,"/Programs/control");assert(child.programs_capability && !child.dosvm_capability);
    supervisor_dosvm_delegate(&parent,&child,"/Programs/dosbox");assert(!child.programs_capability && child.dosvm_capability);
    supervisor_dosvm_delegate(&parent,&child,"/Programs/control-extra");assert(!child.programs_capability && !child.dosvm_capability);
    supervisor_dosvm_delegate(&parent,&child,"/Documents/control");assert(!child.programs_capability);
    grants[1].owner=1;parent.programs_capability=true;
    supervisor_dosvm_delegate(&parent,&child,"/Programs/control");assert(!child.programs_capability);
    supervisor_dosvm_delegate(&parent,&child,"/Programs/dosbox");assert(!child.dosvm_capability);
    return 0;
}
''')
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-I'+str(ROOT/'src/kernel/include'),harness,'-o',binary],check=True)
            subprocess.run([binary],check=True)

    def test_dosvm_harness_links_and_syscalls(self):
        # Use the shared DOSVM generator, not the full kernel script.
        # Both original link profiles use dosvm_test.c's VFS fixture
        # and f3-52's real-uaddr model. Exercise WAIT_PRESENT alongside the
        # management operations so their numeric dispatch stays distinct.
        work=ROOT/'build/host/f2-87';work.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            folder=Path(temporary);env=dict(os.environ,TMPDIR=temporary,CIUKI_VM_WAIT_PRESENT='1')
            subprocess.run(['python3','-B',str(ROOT/'tests/host/vm/prepare_lifecycle.py'),
                            str(ROOT),temporary],check=True,env=env)
            results=[]
            for profile,flags in (('sanitized',['-O1','-fsanitize=address,undefined']),('optimized',['-O2'])):
                pool=subprocess.check_output(['python3','-B',str(ROOT/'tests/host/vm/pool_build.py'),
                                              str(ROOT),temporary,*(['--plain'] if profile=='optimized' else [])],
                                             text=True,env=env).strip()
                objects=[]
                for unit in ('vm_source','opl3','mixer'):
                    obj=folder/(profile+'-'+unit+'.o');objects.append(obj)
                    subprocess.run(['clang','-std=c17',*flags,'-g','-Wall','-Wextra','-Werror',
                                    '-include',str(ROOT/'tests/host/vm_audio_shim.h'),'-I'+str(ROOT/'src/kernel/include'),
                                    '-c',str(ROOT/f'src/kernel/audio/{unit}.c'),'-o',obj],check=True,env=env)
                sources=['tests/host/dosvm_test.c','tests/host/dosvm_uaddr_model.c',
                         'src/kernel/lib/fmt.c','src/kernel/lib/sha256.c',
                         'src/kernel/net/bridge.c','src/kernel/vm/dosvm_ipx.c','src/kernel/vm/dosvm_ne2000.c']
                sources+=['src/kernel/vm/'+name+'.c' for name in ('virtual_vga','virtual_vga_bios','vga_x86','vga_presenter')]
                binary=folder/('dosvm-'+profile)
                subprocess.run(['clang','-std=c17',*flags,'-g','-Wall','-Wextra','-Werror','-Wno-pointer-to-int-cast',
                                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                                '-I'+str(ROOT/'src/kernel/include'),'-I'+temporary,
                                *(str(ROOT/s) for s in sources),*objects,pool,'-pthread',
                                '-Wl,-u,pool_fixture_start','-o',binary],check=True,env=env)
                # dosvm_test.c's VFS fixture opens its COW scratch at the
                # root-relative build/host/ path the kernel host script uses;
                # run from the repository root whatever cwd unittest has.
                results.append(subprocess.run([binary],env=env,cwd=ROOT).returncode)
            self.assertEqual(results,[0,0])

    def test_actual_owner_provider(self):
        work=ROOT/'build/host/f2-87';work.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            folder=Path(temporary);image=folder/'inner.img';outer=folder/'outer.img'
            env=dict(os.environ,TMPDIR=temporary,MTOOLS_SKIP_CHECK='1')
            for disk,size in ((image,16),(outer,32)):
                with disk.open('wb') as stream:stream.truncate(size*1024*1024)
                subprocess.run(['mkfs.fat','--invariant','-F','16','-s','2',disk],check=True,env=env,capture_output=True)
            for name in ('WOLF3D','DOOM','QUAKE'):
                subprocess.run(['mmd','-i',image,'::/'+name],check=True,env=env)
            payload=folder/'data';payload.write_bytes(b'fixture data'*300)
            for name in ('WOLF3D/KEEP.BIN','DOOM/DOOM.EXE','QUAKE/KEEP.BIN'):
                subprocess.run(['mcopy','-i',image,payload,'::/'+name],check=True,env=env)
            for name in ('Games','System','System/DOS'):
                subprocess.run(['mmd','-i',outer,'::/'+name],check=True,env=env)
            subprocess.run(['mcopy','-i',outer,image,'::/System/DOS/UNIFIED.IMG'],check=True,env=env)
            sidecar=folder/'UNIFIED.SHA'
            sidecar.write_text(hashlib.sha256(image.read_bytes()).hexdigest()+'\n')
            subprocess.run(['mcopy','-i',outer,sidecar,'::/System/DOS/UNIFIED.SHA'],check=True,env=env)
            for name in ('G2K0.COW','G2K1.COW','G1K0.COW'):
                subprocess.run(['mcopy','-i',outer,payload,'::/System/DOS/'+name],check=True,env=env)
            binary=folder/'owner-test'
            owner_test_build(binary,env)
            ran=subprocess.run([binary,outer],env=env)
            subprocess.run(['fsck.fat','-n',outer],check=True,env=env)
            subprocess.run(['mcopy','-o','-i',outer,'::/System/DOS/UNIFIED.IMG',image],check=True,env=env)
            subprocess.run(['mcopy','-o','-i',outer,'::/System/DOS/UNIFIED.SHA',sidecar],check=True,env=env)
            self.assertEqual(sidecar.read_text(),hashlib.sha256(image.read_bytes()).hexdigest()+'\n')
            subprocess.run(['fsck.fat','-n',image],check=True,env=env)
            self.assertEqual(ran.returncode,0)
    def test_actual_owner_provider_partitioned(self):
        # fx-4: the canonical UNIFIED.IMG is partitioned like the unified
        # reference (scripts/dos_conformance.py): MBR, one active FAT16
        # partition of type 0Eh at LBA 63. The T23 trial of 9a60215f failed
        # every game launch because the inventory accepted only type 06h.
        work=ROOT/'build/host/f2-87';work.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            folder=Path(temporary);image=folder/'inner.img';outer=folder/'outer.img'
            env=dict(os.environ,TMPDIR=temporary,MTOOLS_SKIP_CHECK='1')
            sectors,start=32768,63
            with image.open('wb') as stream:stream.truncate(sectors*512)
            subprocess.run(['mkfs.fat','--invariant','-F','16','-s','2','-h',str(start),'--offset',str(start),
                            image,str((sectors-start)//2)],check=True,env=env,capture_output=True)
            mbr=bytearray(512)
            mbr[446:462]=bytes([0x80,1,1,0,0x0e,0xfe,0xff,0xff])+start.to_bytes(4,'little')+(sectors-start).to_bytes(4,'little')
            mbr[510:512]=b'\x55\xaa'
            with image.open('r+b') as stream:stream.write(mbr)
            part=f'{image}@@{start*512}'
            for name in ('WOLF3D','DOOM','QUAKE'):
                subprocess.run(['mmd','-i',part,'::/'+name],check=True,env=env)
            payload=folder/'data';payload.write_bytes(b'fixture data'*300)
            for name in ('WOLF3D/KEEP.BIN','DOOM/DOOM.EXE','QUAKE/KEEP.BIN'):
                subprocess.run(['mcopy','-i',part,payload,'::/'+name],check=True,env=env)
            with outer.open('wb') as stream:stream.truncate(32*1024*1024)
            subprocess.run(['mkfs.fat','--invariant','-F','16','-s','2',outer],check=True,env=env,capture_output=True)
            for name in ('Games','System','System/DOS'):
                subprocess.run(['mmd','-i',outer,'::/'+name],check=True,env=env)
            subprocess.run(['mcopy','-i',outer,image,'::/System/DOS/UNIFIED.IMG'],check=True,env=env)
            sidecar=folder/'UNIFIED.SHA'
            sidecar.write_text(hashlib.sha256(image.read_bytes()).hexdigest()+'\n')
            subprocess.run(['mcopy','-i',outer,sidecar,'::/System/DOS/UNIFIED.SHA'],check=True,env=env)
            binary=folder/'owner-test'
            owner_test_build(binary,env)
            ran=subprocess.run([binary,outer,'partitioned'],env=env)
            self.assertEqual(ran.returncode,0)
            subprocess.run(['fsck.fat','-n',outer],check=True,env=env)
            subprocess.run(['mcopy','-o','-i',outer,'::/System/DOS/UNIFIED.IMG',image],check=True,env=env)
            subprocess.run(['mcopy','-o','-i',outer,'::/System/DOS/UNIFIED.SHA',sidecar],check=True,env=env)
            data=image.read_bytes()
            self.assertEqual(sidecar.read_text(),hashlib.sha256(data).hexdigest()+'\n')
            self.assertEqual(data[446:462],mbr[446:462])   # 0Eh restored, table intact
            volume=folder/'partition.img';volume.write_bytes(data[start*512:])
            subprocess.run(['fsck.fat','-n',volume],check=True,env=env)
            listing=subprocess.run(['mdir','-b','-i',part,'::/'],check=True,env=env,capture_output=True,text=True).stdout
            self.assertNotIn('DOOM',listing.upper())
            self.assertIn('QUAKE',listing.upper());self.assertIn('WOLF3D',listing.upper())
def owner_test_build(binary,env):
    sources=['src/kernel/fs/'+name+'.c' for name in ('fs_port','blkreq','cache','path','fat','vfs','fs_ro','program_tree','partition')]
    sources+=['tests/host/fs/fake.c','src/kernel/lib/fmt.c','src/kernel/lib/sha256.c','tests/host/desktop/program_owner_test.c']
    subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L',
                    '-DFS_HOST','-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
                    '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                    '-I'+str(ROOT/'src/kernel/include'),'-I'+str(ROOT/'src/kernel/fs'),
                    *(str(ROOT/s) for s in sources),'-pthread','-o',binary],check=True,env=env)
if __name__=='__main__':unittest.main()

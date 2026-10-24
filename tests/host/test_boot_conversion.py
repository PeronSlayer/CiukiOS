"""G29D: assembled FAT16/FAT32 loader over real converter redo sectors."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from loader_parser_machine import LoaderParserMachine
ROOT = Path(__file__).resolve().parents[2]
SYMBOLS = ('fat_mount', 'disk_raw', 'progress_require', 'fatal', 'boot_info',
           'sector', 'cv_init', 'cv_active', 'disk_read', 'system_cluster',
           'directory_find', 'file_open', 'file_getc', 'file_seek', 'found_size',
           'kernel_name', 'partition_field', 'fat_type', 'cluster_bytes', 'cv_crc')

class BootConversionTests(unittest.TestCase):
    def test_assembled_loader_and_real_redo(self):
        parent=ROOT/'build/host'; parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='boot-convert-',dir=parent) as tmp:
            folder=Path(tmp); asm=folder/'loader.asm'; binary=folder/'symbols.bin'
            asm.write_text('%include "src/boot/ciukldr.asm"\npartition_field equ B(partition_lba)\nsection .symbols align=1\ndw '+','.join(SYMBOLS)+'\n')
            subprocess.run(['nasm','-f','bin',str(asm),'-o',str(binary)],cwd=ROOT,check=True)
            mbr=folder/'mbr.bin'; subprocess.run(['nasm','-f','bin','src/boot/mbr.asm','-o',str(mbr)],cwd=ROOT,check=True)
            loader=bytearray(binary.read_bytes()[:24576]); struct.pack_into('<H',loader,6,48)
            struct.pack_into('<I',loader,8,zlib.crc32(loader[16:])); installed=folder/'loader.bin'; installed.write_bytes(loader)
            helper=folder/'fixture'
            sources=['src/kernel/fs/fs_port.c','src/kernel/fs/blkreq.c','src/kernel/fs/cache.c',
                     'src/kernel/fs/path.c','src/kernel/fs/fat.c','tests/host/fs/fake.c','tests/host/fs/boot_convert_fixture.c']
            subprocess.run(['clang','-DFS_HOST','-D_POSIX_C_SOURCE=200809L','-std=c17','-O1','-g',
                '-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',
                '-Isrc/kernel/include','-Isrc/kernel/fs',*sources,'-o',str(helper)],cwd=ROOT,check=True)
            for spc,sectors in ((2,66640),(16,66640),(64,262720)):
                disk=folder/f'fixed-{spc}.img'
                with disk.open('wb') as out: out.truncate((sectors+2048)*512)
                subprocess.run(['mkfs.fat','-F','16','-s',str(spc),'-h','2048','--offset','2048',str(disk),str(sectors//2)],check=True)
                # This standalone suite keeps the process's sanitizer policy.
                # The filesystem runner explicitly supplies its existing LSan policy.
                def fixture(mode): subprocess.run([str(helper),str(disk),str(mbr),str(installed),mode,str(sectors)],check=True)
                fixture('seed')
                def machine():
                    m=LoaderParserMachine(binary,SYMBOLS); m.budget=50000000
                    m.var('partition_field',2048,32); m.write('dword [fs:0x7ca]',sectors,32)
                    m.hooks[m.symbols['progress_require']]=lambda: None
                    # Qualify the assembled CRC independently below. Use the same
                    # IEEE polynomial's host implementation for large redo scans,
                    # keeping this instruction interpreter's cost bounded.
                    def crc():
                        begin=m.read('ds')*16+m.read('si'); count=m.read('cx')
                        m.write('eax',zlib.crc32(m.mem[begin:begin+count]),32)
                    m.hooks[m.symbols['cv_crc']]=crc
                    def raw():
                        self.assertEqual(m.var('cv_active',bits=8) in (0,1),True)
                        lba=m.read('eax'); self.assertLess(lba,sectors+2048)
                        with disk.open('rb') as source: source.seek(lba*512); data=source.read(512)
                        address=m.read('es')*16+m.read('bx'); m.mem[address:address+512]=data
                    m.hooks[m.symbols['disk_raw']]=raw
                    def bios():
                        self.assertEqual(m.read('ah'),0x41); m.write('bx',0xaa55); m.write('cx',1); m.carry=False
                    m.interrupts[0x13]=bios
                    return m
                def read_image(expected_type):
                    m=machine(); self.assertTrue(m.run('fat_mount')); self.assertEqual(m.var('fat_type',bits=8),expected_type)
                    m.write('eax',m.var('system_cluster',bits=32),32); m.write('si',m.symbols['kernel_name'])
                    self.assertTrue(m.run('directory_find')); self.assertFalse(m.carry)
                    self.assertEqual(m.var('found_size',bits=32),65539); self.assertTrue(m.run('file_open'))
                    for off in (0,511,512,1023,1024,4095,4096,32767,32768,32769,65536,65538):
                        m.write('eax',off,32); self.assertTrue(m.run('file_seek')); self.assertTrue(m.run('file_getc'))
                        self.assertFalse(m.carry); self.assertEqual(m.read('al'),(off*29+(off>>8))&255)
                    m.write('eax',65539,32); self.assertTrue(m.run('file_seek'))
                    self.assertTrue(m.run('file_getc')); self.assertTrue(m.carry)
                    return m
                crc_machine=machine(); del crc_machine.hooks[crc_machine.symbols['cv_crc']]
                for length in (16,28,512):
                    begin=crc_machine.symbols['sector']; pattern=bytes((i*31)&255 for i in range(length))
                    crc_machine.mem[0x20000+begin:0x20000+begin+length]=pattern
                    crc_machine.write('si',begin); crc_machine.write('cx',length)
                    self.assertTrue(crc_machine.run('cv_crc'))
                    self.assertEqual(crc_machine.read('eax'),zlib.crc32(pattern))
                read_image(16)
                fixture('commit'); self.assertEqual(read_image(32).var('cv_active',bits=8),1)
                with disk.open('r+b') as source:
                    source.seek(2048*512); locator=source.read(512); journal,count=struct.unpack_from('<II',locator,100)
                    source.seek((2048+journal)*512+512); original=source.read(512)
                    damaged=bytearray(original); damaged[99]^=1
                    source.seek((2048+journal)*512+512); source.write(damaged)
                self.assertFalse(machine().run('fat_mount')) # every CRC checked before visibility
                with disk.open('r+b') as source:
                    source.seek((2048+journal)*512+512); source.write(original)
                    # Apply half of the transaction; the virtual view is unchanged.
                    for i in range(count//2):
                        source.seek((2048+journal+2*i)*512); header=source.read(512); data=source.read(512)
                        target=struct.unpack_from('<I',header,8)[0]
                        if target!=0xffffffff: source.seek((2048+target)*512); source.write(data)
                read_image(32)
                fixture('recover'); self.assertEqual(read_image(32).var('cv_active',bits=8),0)
                print('PASS assembled FAT16/FAT32 boot reader, startup seeks, committed/partially replayed redo overlay, corrupt-journal refusal and kernel replay')

if __name__ == '__main__': unittest.main()

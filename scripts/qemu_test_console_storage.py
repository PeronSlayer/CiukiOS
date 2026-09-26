#!/usr/bin/env python3
"""Real PS/2 validation of the VBE cell scratch and command-history heaps.

No guest RAM/register writes and no VM stops. Firmware glyphs, cached cells,
MCB allocation bounds and the actual QEMU framebuffer are observed separately.
"""
import argparse,hashlib,json,shutil,struct,subprocess,time
from pathlib import Path
import numpy as np
from PIL import Image
from qemu_test_native_windows import WindowVM
from qemu_test_native_utilities import Utilities
from qemu_test_installed_hdd import FAT16

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

class Console(Utilities):
    def command(self,text,marker=None):
        offset=self.vm.offset(); self.vm.text(text)
        self.vm.wait(marker or 'CiukiOS SHELL C:\\',offset,60)
        self.vm.wait('CiukiOS SHELL C:\\',offset,60)
        self.until(lambda:self.b('vc_active')==1,'graphical console did not resume')
        time.sleep(.15)

    def dump(self,name):
        path=self.vm.output/(name+'.bin')
        self.vm.hmp(f'pmemsave 0 1048576 "{path}"')
        return path.read_bytes()

    def pixels(self,name):
        self.refresh()
        assert self.b('vc_active')==1,'not exercising graphical console'
        segment=self.w('vc_cells_seg'); columns=self.w('vc_cols'); rows=self.w('vc_rows')
        scratch=self.w('vc_cell_off'); cell_bytes=columns*rows*2
        assert segment and scratch==cell_bytes,dict(segment=segment,scratch=scratch,cells=cell_bytes)
        ram=self.dump(name+'-memory')
        allocation=struct.unpack_from('<H',ram,segment*16-16+3)[0]*16
        assert scratch+512<=allocation,dict(scratch=scratch,allocation=allocation)
        font_base=self.w('vc_font_seg')*16+self.w('vc_font_off')
        cells=ram[segment*16:segment*16+cell_bytes]
        font=ram[font_base:font_base+4096]
        assert len(font)==4096
        info=self.ram[self.offset('vc_info'):self.offset('vc_info')+256]
        colors=struct.unpack_from('<16I',self.ram,self.offset('vc_colors'))
        palette=self.ram[self.offset('vc_palette'):self.offset('vc_palette')+48]
        bpp=info[25]
        def rgb(index):
            if bpp==8:
                # QEMU hw/display/vga_int.h c6_to_8 replicates bit zero,
                # unlike usual upper-bit expansion: 42 -> 168, 21 -> 87.
                # https://gitlab.com/qemu-project/qemu/-/blob/master/hw/display/vga_int.h
                return [(v<<2)|((v&1)*3) for v in palette[index*3:index*3+3]]
            value=colors[index]
            return [(value>>info[p+1]&((1<<info[p])-1))*255//((1<<info[p])-1) for p in (31,33,35)]
        path=self.vm.shot(name,(800,600)); frame=np.array(Image.open(path))[:,:,:3]
        Image.fromarray(frame).save(path.with_suffix('.png'))
        after=self.dump(name+'-after')
        assert after[segment*16:segment*16+cell_bytes]==cells,'console changed during framebuffer observation'
        compared=0; visible=[]
        cursor=(self.w('vc_y'),self.w('vc_x'))
        for row in range(1,rows):
            visible.append(bytes(cells[row*columns*2:(row+1)*columns*2:2]).decode('cp437').rstrip())
            for column in range(columns):
                if (row,column)==cursor: continue
                ch,attr=cells[(row*columns+column)*2:(row*columns+column)*2+2]
                glyph=np.array(list(font[ch*16:(ch+1)*16]),dtype=np.uint8)
                mask=(glyph[:,None]&(128>>np.arange(8)))!=0
                expected=np.where(mask[:,:,None],rgb(attr&15),rgb(attr>>4)).astype(np.int16)
                actual=frame[row*16:row*16+16,column*8:column*8+8,:].astype(np.int16)
                assert actual.shape==expected.shape,(row,column,frame.shape,columns,rows)
                difference=np.abs(actual-expected)
                assert difference.max()<=(0 if bpp==8 else 1),f'{name}: row={row} col={column} char={ch} attr={attr:02x} maxRGBerror={difference.max()}'
                compared+=128
        item={'name':name,'mode':self.w('vc_mode'),'bpp':bpp,'linear_framebuffer':self.b('vc_lfb'),'columns':columns,'rows':rows,
              'cell_segment':segment,'cells_bytes':cell_bytes,'scratch_offset':scratch,'scratch_bytes':512,
              'allocation_bytes':allocation,'compared_pixels':compared,'text':'\n'.join(visible)}
        self.events.append(item)
        print(f'[console-storage] {name}: {compared} real framebuffer pixels match firmware glyphs',flush=True)
        return item

    def recall(self,key,expected):
        self.vm.key(key)
        self.until(lambda:self.z('input_buf')==expected,f'history {key}: expected {expected!r}')

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--image',type=Path,required=True)
    ap.add_argument('--shell',type=Path,required=True)
    ap.add_argument('--listing',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args(); out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    disk=out/'test.img';assert not disk.exists(),'preserve previous evidence: choose a new output directory'
    shutil.copyfile(args.image,disk)
    assert FAT16(disk).read('SYSTEM/SHELL.COM')==args.shell.read_bytes(),'image does not contain the given shell'
    shutil.copyfile(args.shell,out/'tested-shell.com');shutil.copyfile(args.listing,out/'tested-shell.lst')
    profile=out/'DISPLAY.CFG';profile.write_bytes(b'0800')
    scroll=out/'SCROLL.TXT'
    scroll.write_bytes(b''.join(f'CONSOLE LINE {i:03d}: ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789\r\n'.encode() for i in range(100)))
    subprocess.run(['mcopy','-o','-i',str(disk),str(profile),'::SYSTEM/VIDEO/DISPLAY.CFG'],check=True)
    subprocess.run(['mcopy','-o','-i',str(disk),str(scroll),'::APPS/SCROLL.TXT'],check=True)
    vm=WindowVM(disk,out,'std',memory=128,palette='platinum'); report={'result':'RUNNING','image_sha256':sha(args.image),'shell_sha256':sha(args.shell)}
    try:
        vm.ready();ui=Console(vm,args.shell.read_bytes(),args.listing)
        offset=vm.offset();vm.key('f4');vm.wait('[DESKTOP] DOS',offset,30)
        ui.command('echo HISTORY ONE','\r\nHISTORY ONE\r\nCiukiOS SHELL')
        long='echo '+('a'*110)
        ui.command(long,'\r\n'+('a'*110)+'\r\nCiukiOS SHELL')
        ui.command('echo HISTORY THREE','\r\nHISTORY THREE\r\nCiukiOS SHELL')
        ui.recall('up','echo HISTORY THREE');ui.recall('up',long)
        ui.recall('down','echo HISTORY THREE');ui.recall('down','')
        ui.command('type \\APPS\\SCROLL.TXT','CONSOLE LINE 099:')
        state=ui.pixels('01-type-scroll')
        assert 'CONSOLE LINE 099:' in state['text'] and 'CONSOLE LINE 000:' not in state['text'],state['text']
        ui.command('dir \\APPS','COMDEMO')
        state=ui.pixels('02-dir')
        assert 'Directory listing' in state['text'] or 'DIR>' in state['text'] or 'COMDEMO' in state['text']
        ui.command('comdemo','COM demo via INT21h')
        ui.pixels('03-child-return')
        offset=vm.offset();vm.text('exit');vm.ready(offset)
        vm.shot('04-desktop-resumed',(800,600)); vm.pointer()
        offset=vm.offset();vm.key('f4');vm.wait('[DESKTOP] DOS',offset,30)
        ui.recall('up','exit');ui.recall('up','comdemo')
        ui.recall('down','exit');ui.recall('down','')
        ui.command('echo AFTER DESKTOP','\r\nAFTER DESKTOP\r\nCiukiOS SHELL')
        ui.pixels('05-console-after-desktop')
        history=[]
        for i in range(10):
            command='echo HISTORY WRAP '+str(i)
            ui.command(command,'\r\nHISTORY WRAP '+str(i)+'\r\nCiukiOS SHELL')
            history.append(command)
        for command in reversed(history[-8:]):ui.recall('up',command)
        ui.recall('up',history[-8])
        for command in history[-7:]:ui.recall('down',command)
        ui.recall('down','')
        ui.pixels('06-history-wrap')
        memory=ui.dump('final-memory');seg=ui.w('history_segment')
        assert seg and struct.unpack_from('<H',memory,seg*16-16+3)[0]*16>=1024
        stored=[bytes(memory[seg*16+i*128:seg*16+(i+1)*128]).split(b'\0')[0].decode('ascii') for i in range(8)]
        assert set(stored)==set(history[-8:]),stored
        report.update(result='PASS',checks=ui.events,history_segment=seg,history_entries=stored,
                      scope='QEMU KVM/PentiumIII model/128MiB, PS2 input, read-only guest memory observation; no physical-hardware claim')
    except BaseException as error:
        report.update(result='FAIL',error=str(error))
        vm.shot('failure');(out/'failure-registers.txt').write_bytes(vm.hmp('info registers'))
        raise
    finally:
        vm.close();(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'result':report['result'],'pixel_checks':len(report['checks']),'history_entries':stored},indent=2))
if __name__=='__main__':main()

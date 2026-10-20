"""Authored codec conformance, malformed-input sanitizers and real PE inventories."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import unittest
from . import fixtures as F
from scripts.win32 import codecs, census

ROOT=Path(__file__).resolve().parents[4]
OUT=ROOT/'build/host/win32/codecs'
PTR=C.c_void_p
U=C.c_uint32
H=C.c_size_t


class Bitmap(C.Structure):
    _fields_=[('size',U),('width',C.c_int32),('height',C.c_int32),('planes',C.c_uint16),('bits',C.c_uint16),('compression',U),('bytes',U),('x',C.c_int32),('y',C.c_int32),('colors',U),('important',U)]


class Header(C.Structure):
    _pack_=1
    _fields_=[('size',U),('status',U),('user',H),('src',PTR),('src_len',U),('src_used',U),('src_user',H),('dst',PTR),('dst_len',U),('dst_used',U),('dst_user',H),('reserved',U*10)]


def buffer(b):
    return C.create_string_buffer(b,len(b))


def four(s):
    return int.from_bytes(s.encode(),'little')


class Codecs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True,exist_ok=True)
        sources=[ROOT/'sdk/win32/codecs'/u for u in ('adpcm.c','pcm.c','video.c','acm.c','vcm.c','avi.c')]+[Path(__file__).with_name('host.c')]
        flags=['-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL','-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'sdk/win32/codecs')]
        subprocess.run(['clang',*flags,'-shared','-fPIC',*map(str,sources),'-o',str(OUT/'native.so')],check=True)
        cls.lib=C.CDLL(str(OUT/'native.so'))
        signatures={
            'acmFormatSuggest':(U,[H,PTR,PTR,U,U]),
            'acmStreamOpen':(U,[C.POINTER(H),H,PTR,PTR,PTR,H,H,U]),
            'acmStreamClose':(U,[H,U]),'acmStreamSize':(U,[H,U,C.POINTER(U),U]),
            'acmStreamPrepareHeader':(U,[H,C.POINTER(Header),U]),
            'acmStreamConvert':(U,[H,C.POINTER(Header),U]),
            'acmStreamUnprepareHeader':(U,[H,C.POINTER(Header),U]),
            'ICOpen':(H,[U,U,U]),'ICClose':(C.c_int32,[H]),
            'ICSendMessage':(C.c_ssize_t,[H,U,H,H]),
            'ICDecompress':(C.c_int32,[H,U,C.POINTER(Bitmap),PTR,C.POINTER(Bitmap),PTR]),
            'test_file':(None,[PTR,U]),'AVIFileOpenA':(C.c_int32,[C.POINTER(PTR),C.c_char_p,U,PTR]),
            'AVIFileGetStream':(C.c_int32,[PTR,C.POINTER(PTR),U,C.c_int32]),
            'AVIFileRelease':(U,[PTR]),'AVIStreamRelease':(U,[PTR]),
            'AVIStreamLength':(C.c_int32,[PTR]),'AVIStreamRead':(C.c_int32,[PTR,C.c_int32,C.c_int32,PTR,C.c_int32,C.POINTER(C.c_int32),C.POINTER(C.c_int32)]),
            'AVIStreamGetFrameOpen':(PTR,[PTR,PTR]),'AVIStreamGetFrame':(PTR,[PTR,C.c_int32]),'AVIStreamGetFrameClose':(C.c_int32,[PTR]),
        }
        for name,(restype,args) in signatures.items():
            func=getattr(cls.lib,name);func.restype=restype;func.argtypes=args
        cls.flags=flags;cls.sources=sources

    def audio(self,wave,src,expected,flags=0x30):
        lib=self.lib;w=buffer(wave);d=C.create_string_buffer(18);h=H()
        self.assertEqual(lib.acmFormatSuggest(0,w,d,18,0),0)
        self.assertEqual(lib.acmStreamOpen(C.byref(h),0,w,d,None,0,0,0),0)
        try:
            dest=C.create_string_buffer(len(expected)+32);source=buffer(src)
            header=Header(size=C.sizeof(Header),src=C.addressof(source),src_len=len(src),dst=C.addressof(dest),dst_len=len(dest))
            self.assertEqual(lib.acmStreamPrepareHeader(h,C.byref(header),0),0)
            self.assertEqual(lib.acmStreamConvert(h,C.byref(header),flags),0)
            self.assertEqual(dest.raw[:header.dst_used],expected)
            self.assertEqual(header.src_used,len(src))
            self.assertEqual(header.status,0x30000)
            self.assertEqual(lib.acmStreamUnprepareHeader(h,C.byref(header),0),0)
        finally:
            self.assertEqual(lib.acmStreamClose(h,0),0)
        self.assertEqual(lib.acmStreamClose(h,0),5)

    def test_adpcm_samples_exact_mono_stereo_rates_blocks_and_final(self):
        for tag in (17,2):
            for channels in (1,2):
                for rate,alignment in ((8000,256),(11025,256),(22050,512),(44100,1024)):
                    alignment*=channels
                    for truncated in (0,1,3,7):
                        with self.subTest(tag=tag,ch=channels,rate=rate,truncate=truncated):
                            wave,block,expected=F.audio(tag,channels,rate,alignment,truncated)
                            self.audio(wave,block,expected)
                    wave,block,expected=F.audio(tag,channels,rate,alignment)
                    self.audio(wave,block*2,expected*2)

    def test_acm_format_constraints_and_size_rounding(self):
        lib=self.lib
        for tag in (17,2):
            wave,block,expected=F.audio(tag,2,44100,2048);w=buffer(wave);d=C.create_string_buffer(18)
            self.assertEqual(lib.acmFormatSuggest(0,w,d,18,0),0)
            self.assertEqual(lib.acmFormatSuggest(0,w,d,16,0xf0000),0)
            for flag,offset,value,fmt in ((0x10000,0,2,'H'),(0x20000,2,1,'H'),(0x40000,4,8000,'I'),(0x80000,14,8,'H')):
                wrong=bytearray(d.raw);struct.pack_into('<'+fmt,wrong,offset,value)
                self.assertEqual(lib.acmFormatSuggest(0,w,buffer(bytes(wrong)),18,flag),512)
            h=H();self.assertEqual(lib.acmStreamOpen(C.byref(h),0,w,d,None,0,0,0),0)
            try:
                for size,flag,code,want in ((0,0,512,0),(2047,0,512,0),(2048,0,0,len(expected)),(2049,0,0,len(expected)*2),(len(expected)-1,1,512,0),(len(expected),1,0,2048),(0xffffffff,0,512,0),(2048,2,11,0)):
                    out=U();self.assertEqual(lib.acmStreamSize(h,size,C.byref(out),flag),code)
                    if not code:self.assertEqual(out.value,want)
            finally:lib.acmStreamClose(h,0)

    def decode(self,kind,bits,key,delta,expected,compression=None,height=4):
        lib=self.lib;h=lib.ICOpen(four('vidc'),four(kind),2);self.assertTrue(h)
        info=(C.c_uint8*568)()
        self.assertEqual(lib.ICSendMessage(h,0x5002,C.addressof(info),568),568)
        self.assertEqual(struct.unpack_from('<I',bytes(info),12)[0],0)
        src=Bitmap(40,4,4,1,bits,compression if compression is not None else four(kind),len(key))
        obits=24 if kind=='cvid' else (8 if kind=='mrle' else bits)
        dest=Bitmap(40,4,height,1,obits,0,4*4*(obits//8));pixels=C.create_string_buffer(dest.bytes)
        try:
            self.assertEqual(lib.ICSendMessage(h,0x400c,C.addressof(src),C.addressof(dest)),0)
            for frame,flags,want in ((key,0,expected),(delta,0x08000000,expected)):
                src.bytes=len(frame);self.assertEqual(lib.ICDecompress(h,flags,C.byref(src),buffer(frame),C.byref(dest),pixels),0)
                self.assertEqual(pixels.raw,want)
            # A truncated source gets a stable error, including missing payloads.
            for n in range(len(key)):
                if not n:continue
                src.bytes=n
                self.assertLess(lib.ICDecompress(h,0,C.byref(src),buffer(key[:n]),C.byref(dest),pixels),0)
            src.bytes=len(key)+1
            self.assertLess(lib.ICDecompress(h,0,C.byref(src),buffer(key+b'\x99'),C.byref(dest),pixels),0)
        finally:self.assertEqual(lib.ICClose(h),0)
        self.assertEqual(lib.ICClose(h),-8)
        return hashlib.sha256(expected).hexdigest()

    def test_video_key_delta_digests(self):
        expected={};key=bytes((4,7,0,0))*3+bytes((4,7,0,1))
        expected['rle8']=self.decode('mrle',8,key,b'\0\1',bytes((7,))*16,1)
        key4=bytes((4,0x12,0,0))*3+bytes((4,0x12,0,1))
        expected['rle4']=self.decode('mrle',4,key4,b'\0\1',bytes((1,2))*8,2)
        expected['msvc8']=self.decode('msvc',8,b'\x34\x80',b'\x01\x84',b'\x34'*16)
        expected['msvc16']=self.decode('msvc',16,b'\x34\x92',b'\x01\x84',b'\x34\x12'*16)
        for gray in (True,False):
            for partial in (False,True):
                colors=[]
                for y in (80,112,16,48):
                    u,v=(0,0) if gray else (20,-10)
                    colors.append(bytes((max(0,min(255,y+2*u)),max(0,min(255,y-((u+1)//2)-v)),max(0,min(255,y+2*v)))))
                want=b''.join(c*2 for c in colors[:2])*2+b''.join(c*2 for c in colors[2:])*2
                label='cvid-'+str(gray)+'-'+str(partial)
                expected[label]=self.decode('cvid',24,F.cinepak(gray=gray,partial=partial),F.cinepak(gray=gray,partial=partial,delta=True),want)
        self.assertEqual(expected,json.loads(Path(__file__).with_name('goldens.json').read_text()))

    def test_rle_absolute_runs_and_delta_coordinates(self):
        # RLE4 absolute 5 pixels is 3 bytes plus one word pad.
        lib=self.lib;h=lib.ICOpen(four('vidc'),four('mrle'),2)
        source=Bitmap(40,8,4,1,4,2,0);dest=Bitmap(40,8,4,1,8,0,32);pixels=C.create_string_buffer(b'\x55'*32,32)
        try:
            self.assertEqual(lib.ICSendMessage(h,0x400c,C.addressof(source),C.addressof(dest)),0)
            data=bytes((0,5,0x12,0x34,0x50,0,0,2,2,1,1,0x60,0,1));source.bytes=len(data)
            self.assertEqual(lib.ICDecompress(h,0,C.byref(source),buffer(data),C.byref(dest),pixels),0)
            self.assertEqual(pixels.raw[:5],bytes((1,2,3,4,5)))
            self.assertEqual(pixels.raw[15],6)
        finally:lib.ICClose(h)

    def test_avi_read_stream_and_decoded_frames(self):
        lib=self.lib;key=bytes((4,7,0,0))*3+bytes((4,7,0,1));data=buffer(F.avi((key,b'\0\1')))
        lib.test_file(data,len(data));file=PTR();stream=PTR()
        self.assertEqual(lib.AVIFileOpenA(C.byref(file),b'authored.avi',0,None),0)
        self.assertEqual(lib.AVIFileGetStream(file,C.byref(stream),four('vids'),0),0)
        lib.AVIFileRelease(file)
        try:
            self.assertEqual(lib.AVIStreamLength(stream),2)
            got=C.create_string_buffer(100);nb=C.c_int32();ns=C.c_int32()
            self.assertEqual(lib.AVIStreamRead(stream,0,1,got,100,C.byref(nb),C.byref(ns)),0)
            self.assertEqual(got.raw[:nb.value],key);self.assertEqual(ns.value,1)
            desired=Bitmap(40,4,4,1,8,0,0)
            getter=lib.AVIStreamGetFrameOpen(stream,C.byref(desired));self.assertTrue(getter)
            try:
                for n in (0,1,0,1):
                    p=lib.AVIStreamGetFrame(getter,n);self.assertTrue(p)
                    self.assertEqual(C.string_at(p+40+1024,16),b'\x07'*16)
            finally:self.assertEqual(lib.AVIStreamGetFrameClose(getter),0)
            getter=lib.AVIStreamGetFrameOpen(stream,1);self.assertTrue(getter)
            try:self.assertTrue(lib.AVIStreamGetFrame(getter,0))
            finally:self.assertEqual(lib.AVIStreamGetFrameClose(getter),0)
        finally:lib.AVIStreamRelease(stream)

    def test_sanitized_malformed_input(self):
        exe=OUT/'fuzz'
        key=bytes((4,7,0,0))*3+bytes((4,7,0,1))
        seeds=[key,b'\x34\x92',F.cinepak(gray=True),F.avi((key,b'\0\1'))]
        (OUT/'authored.h').write_text('\n'.join('static const unsigned char seed%d[]={%s};'%(i,','.join(map(str,s))) for i,s in enumerate(seeds)))
        subprocess.run(['clang',*self.flags,'-I'+str(OUT),'-fsanitize=address,undefined','-fno-sanitize-recover=all',*map(str,self.sources),str(Path(__file__).with_name('fuzz.c')),'-o',str(exe)],check=True)
        p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=120,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(p.returncode,0,p.stdout+p.stderr);self.assertIn('PASS 24000',p.stdout)

    def test_reproducible_pe_imports_coverage_cpu_and_residency(self):
        a=codecs.build(OUT/'a');b=codecs.build(OUT/'b')
        self.assertEqual(a,b)
        total=0
        for row in a:
            pe=census.PE((OUT/'a'/row['name']).read_bytes());total+=pe.size
            self.assertEqual(row['reserved_image_pages'],(pe.size+4095)//4096)
            self.assertEqual(row['resident_pages_store_model'],row['shared_seed_pages']+row['private_cow_credit_pages'])
            self.assertEqual(set(row['coverage']),{n for e in pe.exports() for n in e['names']})
            for imported in pe.imports():
                self.assertIn(imported['dll'].lower(),('kernel32.dll','msvfw32.dll'))
                self.assertNotIn('__wine',str(imported))
            asm=subprocess.run(['llvm-objdump','-d',str(OUT/'a'/row['name'])],capture_output=True,text=True,check=True).stdout
            self.assertNotRegex(asm,r'\b(?:xmm\d|mm[0-7])\b')
        self.assertLess(total,5*1024*1024)

    def test_store_t1_hash_license_and_production_loader_binding(self):
        from scripts.win32 import builtins,stubs
        from tests.host.win32.adapter import build as adapter
        directory=OUT/'store'
        manifest=builtins.build(directory)
        store=json.loads(manifest.read_text());stubs.verify_store(manifest)
        evidence=json.loads((directory/'codecs-inventory.json').read_text())
        self.assertFalse(any(e['missing'] for entries in evidence['imports'].values() for e in entries))
        original=manifest.read_bytes()
        try:
            changed=json.loads(original)
            next(d for d in changed['dlls'] if d['name']=='msacm32.dll')['license']='wrong'
            manifest.write_text(json.dumps(changed))
            with self.assertRaises(ValueError):stubs.verify_store(manifest)
            changed=json.loads(original);changed['dlls']=[d for d in changed['dlls'] if d['name']!='iccvid.dll']
            manifest.write_text(json.dumps(changed))
            with self.assertRaises(ValueError):stubs.verify_store(manifest)
        finally:manifest.write_bytes(original)
        image=directory/'imaadp32.acm';original=image.read_bytes()
        try:
            image.write_bytes(original[:-1]+bytes((original[-1]^1,)))
            with self.assertRaisesRegex(ValueError,'SHA256'):stubs.verify_store(manifest)
        finally:image.write_bytes(original)
        # Register real builtins at their linked addresses on the existing H0
        # loader adapter. Calls are observed; no PE instructions execute here.
        declarations=[]
        for d in store['dlls']:
            declarations.append('{'+','.join((json.dumps(d['name']),json.dumps(str(directory/d['path'])),json.dumps(d['sha256']),hex(d['base']),str(d['size']),'{'+','.join(map(str,d['version']))+'}'))+'}')
        names=[d['name'] for d in evidence['dlls']]
        main=OUT/'load.c'
        main.write_text('#include "linux.h"\nstatic struct w32_loader loader;\nstatic struct w32_builtin builtins[]={'+','.join(declarations)+'};\nint host_main(int argc,char **argv){(void)argc;(void)argv;w32_ldr_init(&loader,&host.services,&host.peb);w32_ldr_bind(&loader);loader.builtins=builtins;loader.builtin_count=sizeof(builtins)/sizeof(builtins[0]);if(!w32_ldr_thread_attach(&loader,&host.teb))return 1;const char *names[]={'+','.join(map(json.dumps,names))+'};for(unsigned i=0;i<8;i++){struct w32_module *module;if(!w32_ldr_load(&loader,names[i],0,&module)){host_print(loader.error.reason);return 2;}if(module->pe.entry)return 3;}w32_ldr_shutdown(&loader);if(host.peb.ProcessHeap)RtlDestroyHeap(host.peb.ProcessHeap);if(host.allocations||host.mappings)return 4;host_print("PASS P5 H0 production loader map/bind/unload (no PE execution)\\n");return 0;}\n')
        exe=adapter.build_model(OUT/'loader',main)
        p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr);self.assertIn('PASS P5 H0',p.stdout)

    def test_driver_messages_and_i386_layout(self):
        # Driver ABI is exercised separately from the public ACM/VCM facades.
        class Stream(C.Structure):
            _fields_=[('size',U),('src',PTR),('dst',PTR),('filter',PTR),('callback',H),('instance',H),('open',U),('flags',U),('driver',H),('stream',H)]
        for tag in (17,2):
            name=OUT/('driver'+str(tag)+'.so')
            subprocess.run(['clang',*self.flags,'-shared','-fPIC','-DCC_AUDIO_DRIVER='+str(tag),*map(str,self.sources),'-o',str(name)],check=True)
            lib=C.CDLL(str(name));driver=lib.DriverProc;driver.restype=C.c_ssize_t;driver.argtypes=[H,H,U,H,H]
            wave,block,expected=F.audio(tag,2,22050,1024);w=buffer(wave);d=C.create_string_buffer(18)
            self.assertEqual(self.lib.acmFormatSuggest(0,w,d,18,0),0)
            stream=Stream(size=C.sizeof(Stream),src=C.addressof(w),dst=C.addressof(d))
            self.assertEqual(driver(0,0,3,0,0),tag)
            self.assertEqual(driver(tag,0,0x604c,C.addressof(stream),0),0)
            source=buffer(block);dest=C.create_string_buffer(len(expected))
            header=Header(size=C.sizeof(Header),src=C.addressof(source),src_len=len(block),dst=C.addressof(dest),dst_len=len(dest));header.reserved[0]=0x30
            self.assertEqual(driver(tag,0,0x604f,C.addressof(stream),C.addressof(header)),0)
            self.assertEqual(dest.raw,expected)
            self.assertEqual(driver(tag,0,0x604d,C.addressof(stream),0),0)
            self.assertEqual(driver(tag,0,0x607f,0,0),8)
        for kind,bits,compression,frame in (('mrle',8,1,bytes((4,7,0,0))*3+bytes((4,7,0,1))),('msvc',16,four('msvc'),b'\x34\x92'),('cvid',24,four('cvid'),F.cinepak())):
            name=OUT/(kind+'-driver.so')
            subprocess.run(['clang',*self.flags,'-shared','-fPIC','-DCC_VIDEO_DRIVER='+str(four(kind)),*map(str,self.sources),'-o',str(name)],check=True)
            lib=C.CDLL(str(name));driver=lib.DriverProc;driver.restype=C.c_ssize_t;driver.argtypes=[H,H,U,H,H]
            token=driver(0,0,3,0,0);self.assertGreater(token,0)
            source=Bitmap(40,4,4,1,bits,compression,len(frame));outbits=24 if kind=='cvid' else bits;dest=Bitmap(40,4,4,1,outbits,0,16*outbits//8)
            self.assertEqual(driver(token,0,0x400c,C.addressof(source),C.addressof(dest)),0)
            self.assertEqual(driver(token,0,0x400e,0,0),0)
            self.assertEqual(driver(token,0,4,0,0),1)
        assertions=OUT/'layout.c'
        assertions.write_text('#include "codec.h"\n_Static_assert(sizeof(struct cc_wave)==18,"WAVEFORMATEX");\n_Static_assert(sizeof(struct cc_acm_header)==84,"ACMSTREAMHEADER");\n_Static_assert(sizeof(struct cc_drvstream)==40,"ACMDRVSTREAMINSTANCE");\n_Static_assert(sizeof(struct cc_icinfo)==568,"ICINFO");\n_Static_assert(sizeof(struct cc_bitmap)==40,"BITMAPINFOHEADER");\n_Static_assert(sizeof(struct cc_decompress)==24,"ICDECOMPRESS");\n')
        subprocess.run(['clang',*codecs.FLAGS,'-I'+str(ROOT/'sdk/win32/codecs'),'-c',str(assertions),'-o',str(OUT/'layout.obj')],check=True)

    def test_selected_wine_assertions_on_native_adapter(self):
        # Portable assertions from pinned msacm32/tests/msacm.c:1144ff and
        # msvfw32/tests/msvfw.c:409ff. Not the complete upstream PE test suite.
        lib=self.lib
        pcm=struct.pack('<HHIIHHH',1,1,8000,16000,2,16,0)
        for flags in (0,0xf0000):
            dest=buffer(pcm)
            self.assertEqual(lib.acmFormatSuggest(0,buffer(pcm),dest,16,flags),0)
            self.assertEqual(dest.raw[:16],pcm[:16])
        callback=C.CFUNCTYPE(C.c_int,H,H,U);seen=[]
        def collect(driver,user,flags):
            seen.append((driver,flags));return 1
        self.assertEqual(lib.acmDriverEnum(callback(collect),H(0),U(0)),0)
        self.assertEqual(seen,[(17,1),(2,1),(1,2)])
        for driver,_ in seen:
            opened=H();self.assertEqual(lib.acmDriverOpen(C.byref(opened),H(driver),U(0)),0)
            self.assertEqual(opened.value,driver);self.assertEqual(lib.acmDriverClose(H(driver),U(0)),0)
        raw=(C.c_uint8*568)();lib.ICInfo.argtypes=[U,U,PTR];lib.ICInfo.restype=C.c_int
        count=0
        while lib.ICInfo(0,count,raw):
            fields=struct.unpack_from('<6I',bytes(raw));self.assertNotEqual(fields[1],0);self.assertNotEqual(fields[2],0)
            self.assertEqual(fields[3:6],(0,0,0x104));self.assertEqual(bytes(raw)[24:312],b'\0'*288)
            alternate=(C.c_uint8*568)()
            self.assertTrue(lib.ICInfo(fields[1],fields[2]^0x20,alternate))
            count+=1
        self.assertEqual(count,3)
        self.assertFalse(lib.ICInfo(four('vidc'),four('fake'),raw))

    def test_selected_wine_assertions_as_native_c(self):
        exe=OUT/'wine-native'
        subprocess.run(['clang',*self.flags,*map(str,self.sources),str(Path(__file__).with_name('wine_cases.c')),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)

    def test_selected_wine_assertions_as_i386_pe(self):
        import signal
        from tests.host.win32.adapter import build as adapter
        # Build the actual selected conformance test as PE even when the
        # sandbox rejects the i386 adapter's FS/bootstrap system calls.
        out=OUT/'wine-pe';out.mkdir(exist_ok=True)
        acm=codecs.import_library(out,'msacm32',{'acmFormatSuggest':20})
        vcm=codecs.import_library(out,'msvfw32',{'ICInfo':12})
        obj=out/'cases.obj';probe=out/'cases.exe'
        subprocess.run(['clang',*codecs.FLAGS,'-I'+str(ROOT/'sdk/win32/codecs'),'-c',str(Path(__file__).with_name('wine_cases.c')),'-o',str(obj)],check=True)
        subprocess.run(['lld-link','/machine:x86','/entry:entry@0','/nodefaultlib','/safeseh:no','/timestamp:0','/base:0x00400000','/subsystem:console,4.0','/out:'+str(probe),str(obj),str(acm),str(vcm)],check=True)
        # The store test builds canonical P5 PE files and registers their
        # actual manifest entries for H0. Make an H1 main with the same entries.
        main=OUT/'load.c'
        self.test_store_t1_hash_license_and_production_loader_binding()
        source=main.read_text()
        source=source.replace('if(module->pe.entry)return 3;', 'if(module->pe.entry)return 3;')
        marker='w32_ldr_shutdown(&loader);'
        execute='struct w32_module *test;if(!w32_ldr_load(&loader,'+json.dumps(str(probe))+',0,&test))return 6;void *stack=host.services.alloc(&host,1024*1024);if(!stack)return 7;int result=host_entry(test->pe.image+test->pe.entry,(unsigned char *)stack+1024*1024);host.services.free(&host,stack,1024*1024);if(result)return 20+result;'
        source=source.replace(marker,execute+marker).replace('PASS P5 H0','PASS P5 H1')
        h1main=out/'load.c';h1main.write_text(source)
        exe=adapter.build(out/'adapter',h1main,windows=False)
        p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
        if p.returncode==-signal.SIGSYS:
            self.skipTest('Selected Wine assertion PE built; i386 Linux adapter bootstrap rejected by seccomp (SIGSYS); native C assertions pass separately')
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)

    def test_video_multicolor_and_cinepak_v4(self):
        lib=self.lib
        cases=[(8,b'\x01\0'+bytes((7,9)),bytes((7,))+bytes((9,))*15),
               (8,b'\0\x90'+bytes(range(1,9)),bytes((2,2,4,4,2,2,4,4,6,6,8,8,5,6,8,7))),
               (16,b'\0\0'+struct.pack('<8H',0x8001,2,3,4,5,6,7,8),struct.pack('<16H',2,2,4,4,2,2,4,4,6,6,8,8,6,6,8,8))]
        for bits,frame,want in cases:
            h=lib.ICOpen(four('vidc'),four('msvc'),2)
            src=Bitmap(40,4,4,1,bits,four('msvc'),len(frame));dst=Bitmap(40,4,4,1,bits,0,len(want));pixels=C.create_string_buffer(len(want))
            try:
                self.assertEqual(lib.ICSendMessage(h,0x400c,C.addressof(src),C.addressof(dst)),0)
                self.assertEqual(lib.ICDecompress(h,0,C.byref(src),buffer(frame),C.byref(dst),pixels),0)
                self.assertEqual(pixels.raw,want)
            finally:lib.ICClose(h)
        book=struct.pack('>HH',0x2400,20)+bytes((20,))*4+bytes((40,))*4+bytes((60,))*4+bytes((80,))*4
        pixels=struct.pack('>HHI4B',0x3000,12,0x80000000,0,1,2,3)
        strip=struct.pack('>HHHHHH',0x1000,12+len(book)+len(pixels),0,0,4,4)+book+pixels
        frame=bytes((0,))+(10+len(strip)).to_bytes(3,'big')+struct.pack('>HHH',4,4,1)+strip
        h=lib.ICOpen(four('vidc'),four('cvid'),2)
        src=Bitmap(40,4,4,1,24,four('cvid'),len(frame));dst=Bitmap(40,4,4,1,24,0,48);out=C.create_string_buffer(48)
        try:
            self.assertEqual(lib.ICSendMessage(h,0x400c,C.addressof(src),C.addressof(dst)),0)
            self.assertEqual(lib.ICDecompress(h,0,C.byref(src),buffer(frame),C.byref(dst),out),0)
            self.assertEqual(out.raw,bytes((60,))*6+bytes((80,))*6+bytes((60,))*6+bytes((80,))*6+bytes((20,))*6+bytes((40,))*6+bytes((20,))*6+bytes((40,))*6)
        finally:lib.ICClose(h)

"""Production optical parser and canonical-runner optical host evidence."""
import unittest
import subprocess
import tempfile
import shutil
import hashlib
import sys
import os
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
import iso_fixtures
import run as runner

BLOCK=2048

def both(data,offset,value,width=4):
    data[offset:offset+2*width]=value.to_bytes(width,'little')+value.to_bytes(width,'big')

def descriptors(data,session=0):
    result=[]
    for lba in range(session+16,min(session+144,len(data)//BLOCK)):
        offset=lba*BLOCK
        result.append(offset)
        if data[offset]==255: break
    return result

def directory_records(data,descriptor):
    """Enumerate each hierarchy without using the production ISO parser."""
    root=descriptor+156
    pending=[root]; seen=set()
    yield root
    while pending:
        record=pending.pop()
        base=int.from_bytes(data[record+2:record+6],'little')*BLOCK
        size=int.from_bytes(data[record+10:record+14],'little')
        if base in seen: continue
        seen.add(base)
        offset=0
        while offset<size:
            at=base+offset; length=data[at]
            if not length:
                offset=(offset//BLOCK+1)*BLOCK
                continue
            yield at
            if data[at+25]&2: pending.append(at)
            offset+=length

def multisession_fixture(directory,index):
    """Real xorriso merge, with old absolute extents and a new descriptor set.

    This models the candidate disc structure in f1-54, not a dump or a claim
    about the private disc's exact contents. Rock Ridge and El Torito are
    confined to this host fixture; the canonical Joliet generator is intact.
    """
    directory=Path(directory)
    previous=iso_fixtures.generate({'level':3,'joliet':True},directory,index)
    source=directory/f'session-source-{index}'
    extracted=directory/f'session-extracted-{index}'
    tail=directory/f'session-tail-{index}.iso'
    image=directory/f'multisession-{index}.iso'
    session=512
    try:
        source.mkdir(); extracted.mkdir()
        (source/'NEW.TXT').write_bytes(b'Last session: Ciuki F1-54.\n')
        # Data-only boot image: no boot execution is part of this host test.
        (source/'boot.img').write_bytes(bytes(range(256))*16)
        for p in [*source.iterdir(),source]: os.utime(p,(iso_fixtures.EPOCH,iso_fixtures.EPOCH))
        iso_fixtures.command(['xorriso','-as','mkisofs','-iso-level','3','-R','-J',
                              '-C',f'0,{session}','-M',previous['path'],'-V','CIUKI_F154',
                              '--modification-date='+iso_fixtures.DATE,'--set_all_file_dates',iso_fixtures.DATE,
                              '-uid','0','-gid','0','-b','boot.img','-no-emul-boot','-boot-load-size','4',
                              '--boot-catalog-hide','-o',str(tail),str(source)])
        with image.open('wb') as out, Path(previous['path']).open('rb') as old, tail.open('rb') as new:
            shutil.copyfileobj(old,out)
            if out.tell()>session*BLOCK: raise ValueError('previous session overlaps new fixture')
            out.seek(session*BLOCK); shutil.copyfileobj(new,out)
        # Disable RR to independently extract the hierarchy Ciuki selects.
        iso_fixtures.command(['xorriso','-read_fs','norock','-load','sbsector',str(session),
                              '-indev',str(image),'-osirrox','on','-extract','/',str(extracted)])
        entries=[]
        for p in sorted(extracted.rglob('*')):
            name='/'+p.relative_to(extracted).as_posix()
            entries.append({'path':name,'kind':'directory' if p.is_dir() else 'file',
                            **({} if p.is_dir() else {'size':p.stat().st_size,'sha256':iso_fixtures.sha(p)})})
        return {'path':str(image),'session':session,'entries':entries}
    finally:
        for tree in (source,extracted):
            if tree.exists():
                for p in [tree,*tree.rglob('*')]:
                    if p.is_dir(): p.chmod(0o700)
                shutil.rmtree(tree)
        tail.unlink(missing_ok=True)

def root_record_fixture(directory, quirks=(), joliet=False):
    """Small independently encoded ISO; no mastering/extraction tool needed."""
    data=bytearray(48*BLOCK)
    payload=b'Ciuki F1-58 directory record fixture.\n'
    data[44*BLOCK:44*BLOCK+len(payload)]=payload
    entries=[]

    def record(extent,size,name,flags=0,extended=0):
        length=33+len(name)+(not (len(name)&1))
        r=bytearray(length); r[0]=length; r[1]=extended
        both(r,2,extent); both(r,10,size); both(r,28,1,2)
        r[25]=flags; r[32]=len(name); r[33:33+len(name)]=name
        if 'mismatch' in quirks:
            r[6:10]=bytes(4); r[14:18]=bytes(4); r[30:32]=bytes(2)
        return r

    def hierarchy(descriptor,root,nested,unicode_names):
        size=BLOCK+512 if 'unaligned' in quirks else BLOCK
        extended=1 if 'extended' in quirks else 0
        encode=lambda s: s.encode('utf-16-be' if unicode_names else 'ascii')
        b=bytearray(BLOCK); b[0:7]=bytes([2 if unicode_names else 1])+b'CD001\1'
        b[40:72]=b'CIUKI_F158'.ljust(32,b' ')
        both(b,80,len(data)//BLOCK); both(b,120,1,2); both(b,124,1,2); both(b,128,BLOCK,2)
        if unicode_names: b[88:91]=b'%/E'
        b[881]=1; b[156:190]=record(root,size,b'\0',2,extended)
        data[descriptor*BLOCK:(descriptor+1)*BLOCK]=b
        for extent,parent,is_root in ((root,root,True),(nested,root,False)):
            start=(extent+extended)*BLOCK
            # Non-record attribute bytes ensure skipping is actually exercised.
            if extended: data[extent*BLOCK:(extent+1)*BLOCK]=b'X'*BLOCK
            records=[record(extent,size,b'\0',2,extended),record(parent,size,b'\1',2,extended)]
            if is_root: records.append(record(nested,size,encode('SUB'),2,extended))
            name='FIRST.TXT' if is_root else 'NESTED.TXT'
            if unicode_names: name='Joliet Ω.txt' if is_root else 'Nested Ω.txt'
            records.append(record(44,len(payload),encode(name+';1')))
            offset=0
            for r in records: data[start+offset:start+offset+len(r)]=r; offset+=len(r)
            if not joliet or unicode_names:
                prefix='' if is_root else '/SUB'
                entries.append({'path':prefix+'/'+name,'kind':'file','size':len(payload),
                                'sha256':hashlib.sha256(payload).hexdigest()})
                if is_root: entries.append({'path':'/SUB','kind':'directory'})
            if 'unaligned' in quirks:
                r=record(44,len(payload),encode('TAIL.TXT;1'))
                # Starts inside the declared tail, ends in its rounded block.
                dot=record(extent,size,b'\0',2,extended)
                at=start+BLOCK
                for _ in range(14): data[at:at+len(dot)]=dot; at+=len(dot)
                data[at:at+len(r)]=r
                if not joliet or unicode_names:
                    entries.append({'path':('' if is_root else '/SUB')+'/TAIL.TXT','kind':'file',
                                    'size':len(payload),'sha256':hashlib.sha256(payload).hexdigest()})

    hierarchy(16,24,28,False)
    if joliet: hierarchy(17,34,38,True)
    terminator=18 if joliet else 17
    data[terminator*BLOCK:terminator*BLOCK+7]=b'\xffCD001\1'
    image=Path(directory)/('root-record-'+str(joliet)+'-'+'-'.join(quirks)+'.iso')
    image.write_bytes(data)
    return {'path':str(image),'entries':entries}


class OpticalHarness(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('clang'): raise unittest.SkipTest('optical host compiler absent')
        out=ROOT/'build/host/optical'
        out.mkdir(parents=True,exist_ok=True)
        cls.binary=out/'test_optical'
        sources=['tests/host/fs/test_optical.c','src/kernel/fs/fs_port.c','src/kernel/fs/blkreq.c','src/kernel/fs/cache.c',
                 'src/kernel/fs/path.c','src/kernel/fs/fat.c','src/kernel/fs/vfs.c',
                 'src/kernel/fs/fs_ro.c','src/kernel/fs/iso9660.c','src/kernel/lib/sha256.c']
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        '-DFS_HOST','-D_POSIX_C_SOURCE=200809L','-pthread','-Isrc/kernel/include','-Isrc/kernel/fs',
                        *sources,'-o',str(cls.binary)],cwd=ROOT,check=True,capture_output=True)
        cls.temp=tempfile.TemporaryDirectory(dir=out,prefix='fixtures-')
        cls.directory=Path(cls.temp.name)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def check_image(self,manifest,refused=False,sector_size=BLOCK,sectors=None):
        before=iso_fixtures.sha(manifest['path'])
        args=[str(self.binary),manifest['path'],str(manifest.get('session',0)),str(sector_size)]
        if sectors is not None: args.append(str(sectors))
        result=subprocess.run(args,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
        self.assertEqual(result.returncode,0,result.stdout+'\n'+result.stderr)
        self.assertEqual(before,iso_fixtures.sha(manifest['path']))
        lines=result.stdout.splitlines()
        if refused:
            self.assertTrue(any(s.startswith('REFUSED\t') for s in lines),result.stdout+result.stderr)
        else:
            self.assertFalse(any(s.startswith('REFUSED') for s in lines),result.stdout+result.stderr)
            actual={}
            for line in lines:
                fields=line.split('\t')
                if fields[0]=='D': actual[fields[1]]={'kind':'directory'}
                if fields[0]=='F': actual[fields[1]]={'kind':'file','size':int(fields[2]),'sha256':fields[3]}
            expected={e['path']:{k:v for k,v in e.items() if k in ('kind','size','sha256')} for e in manifest['entries']}
            self.assertEqual(expected,actual)
            self.assertTrue(any(s.startswith('COUNTERS\twrites=0\tflushes=0') for s in lines))
        return result.stdout+result.stderr


class RootRecords(OpticalHarness):
    def test_python_directory_mastering_mounts_both_hierarchies(self):
        source=self.directory/'python-source'
        source.mkdir()
        (source/'README.TXT').write_bytes(b'Ciuki F5-17 authored fixture.\n')
        (source/'Caf\u00e9').mkdir()
        (source/'Caf\u00e9'/'Long optical title.txt').write_bytes(bytes(range(256))*33)
        (source/'EMPTY.BIN').write_bytes(b'')
        (source/'PAYLOAD.BIN').write_bytes(bytes(range(256))*2400)
        (source/'ZEMPTY.BIN').write_bytes(b'')
        for n in range(90): (source/f'F{n:03}.TXT').write_bytes(str(n).encode())
        for joliet in (False,True):
            with self.subTest(joliet=joliet):
                a=iso_fixtures.generate_directory(source,self.directory/f'python-{joliet}.iso',joliet)
                b=iso_fixtures.generate_directory(source,self.directory/f'python-{joliet}-repeat.iso',joliet)
                self.assertEqual(a['sha256'],b['sha256'])
                self.check_image(a)

    def test_python_root_quirks_and_nested_records(self):
        for joliet in (False,True):
            for quirks in ((),('mismatch',),('unaligned',),('extended',),('mismatch','unaligned','extended')):
                with self.subTest(joliet=joliet,quirks=quirks):
                    a=root_record_fixture(self.directory,quirks,joliet)
                    output=self.check_image(a)
                    if 'mismatch' in quirks:
                        for field in ('extent','data_length','volume_sequence_number'):
                            self.assertIn(f'field={field} result=0',output)
                            # Harness remounts once: exactly one per mount lifetime.
                            self.assertEqual(sum(f'field={field} result=0' in line and
                                                 'reason=endian_mismatch' in line for line in output.splitlines()),2)
                    if 'unaligned' in quirks:
                        self.assertIn('le=2560 be=',output)
                        self.assertIn('align=2048',output)
                        self.assertEqual(output.count('reason=directory_rounding'),2)

    def test_impossible_le_values_and_raw_diagnostics(self):
        for joliet in (False,True):
            for offset,width,value,field in ((10,4,0,'data_length'),(10,4,0xffffffff,'data_length'),
                                             (2,4,49,'extent')):
                with self.subTest(joliet=joliet,field=field,value=value):
                    a=root_record_fixture(self.directory,(),joliet)
                    p=Path(a['path']); data=bytearray(p.read_bytes())
                    at=(17 if joliet else 16)*BLOCK+156+offset
                    be=int.from_bytes(data[at+width:at+2*width],'big')
                    data[at:at+width]=value.to_bytes(width,'little'); p.write_bytes(data)
                    output=self.check_image(a,True)
                    line=next(line for line in output.splitlines() if f'field={field} result=-5' in line)
                    self.assertIn(f'le={value} be={be}',line)
                    for operand in ('min=','max=','align=','available='): self.assertIn(operand,line)
                    self.assertEqual(output.count(f'field={field} result=-5'),1)

    def test_directory_volume_sequence_zero_mounts(self):
        for joliet in (False,True):
            with self.subTest(joliet=joliet):
                a=root_record_fixture(self.directory,(),joliet)
                p=Path(a['path']); data=bytearray(p.read_bytes())
                both(data,(17 if joliet else 16)*BLOCK+156+28,0,2); p.write_bytes(data)
                self.assertIn('field=volume_sequence_number result=0 le=0',self.check_image(a))

    def test_sub_block_directory_and_readable_end(self):
        a=root_record_fixture(self.directory)
        p=Path(a['path']); data=bytearray(p.read_bytes()); original=bytes(data)
        for at in (16*BLOCK+156,24*BLOCK,24*BLOCK+34): both(data,at+10,512)
        p.write_bytes(data); self.check_image(a)
        # Extended attributes and rounded final blocks must fit the mount end.
        for extended,size in ((0,2049),(1,1),(255,1)):
            data=bytearray(original); data[16*BLOCK+157]=extended
            both(data,16*BLOCK+158,47); both(data,16*BLOCK+166,size)
            p.write_bytes(data); output=self.check_image(a,True)
            self.assertIn('result=-5 le=',output)
        data=bytearray(original)
        both(data,16*BLOCK+80,0)  # valid BE cannot rescue an impossible LE count
        data[16*BLOCK+84:16*BLOCK+88]=(48).to_bytes(4,'big')
        p.write_bytes(data)
        self.assertIn('field=volume_space_size result=-5 le=0 be=48',self.check_image(a,True))

    def test_later_records_cannot_escape_bounds(self):
        for joliet in (False,True):
            for field,offset,value in (('extent',2,49),('data_length',10,0)):
                with self.subTest(joliet=joliet,field=field):
                    a=root_record_fixture(self.directory,(),joliet)
                    p=Path(a['path']); data=bytearray(p.read_bytes())
                    # SUB is the third entry, after dot and dot-dot.
                    at=(34 if joliet else 24)*BLOCK+68+offset
                    data[at:at+4]=value.to_bytes(4,'little'); p.write_bytes(data)
                    output=self.check_image(a,True)
                    self.assertIn(f'descriptor=directory_record field={field} result=-5 le={value} be=',output)
                    self.assertIn('range_rejections=0',output)

    def test_last_readable_block_and_mount_tail(self):
        a=root_record_fixture(self.directory)
        p=Path(a['path']); data=bytearray(p.read_bytes())
        data[47*BLOCK:48*BLOCK]=data[24*BLOCK:25*BLOCK]
        both(data,16*BLOCK+158,47); p.write_bytes(data)
        self.check_image(a)  # exactly one complete block fits
        output=self.check_image(a,True,sectors=47)
        self.assertIn('field=data_length result=-5 le=2048 be=2048 min=1 max=0 align=1 available=0',output)
        both(data,16*BLOCK+166,2049); p.write_bytes(data)
        output=self.check_image(a,True)
        self.assertIn('field=data_length result=-5 le=2049 be=2049 min=1 max=2048 align=1 available=1',output)


class Optical(OpticalHarness):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('xorriso'): raise unittest.SkipTest('optical host mastering tool absent')
        super().setUpClass()

    def test_levels_joliet_determinism(self):
        for level in (1,2,3):
            for joliet in (False,True):
                with self.subTest(level=level,joliet=joliet):
                    spec={'level':level,'joliet':joliet}
                    a=iso_fixtures.generate(spec,self.directory,level*10+joliet)
                    b=iso_fixtures.generate(spec,self.directory,level*10+joliet+100)
                    self.assertEqual(a['sha256'],b['sha256'])
                    self.check_image(a)

    def test_malformed_descriptor(self):
        a=iso_fixtures.generate({'malformed':True},self.directory,300)
        self.assertIn('descriptor=pvd field=extent result=-5',self.check_image(a,True))
        a=iso_fixtures.generate({'joliet':True},self.directory,301)
        p=Path(a['path']); original=p.read_bytes()
        cases=[(16,80,4,0,'pvd','volume_space_size'),
               (16,128,2,512,'pvd','logical_block_size'),
               (17,128,2,512,'svd','logical_block_size'),
               (17,80,4,0,'svd','volume_space_size')]
        for lba,offset,width,value,descriptor,field in cases:
            with self.subTest(field=field,descriptor=descriptor):
                data=bytearray(original); both(data,lba*BLOCK+offset,value,width); p.write_bytes(data)
                self.assertIn(f'descriptor={descriptor} field={field} result=-5',self.check_image(a,True))
        # Volume set fields are informative, as in Linux isofs: any value mounts.
        for offset,value,field in ((120,2,'volume_set_size'),(124,0,'volume_sequence_number'),(124,2,'volume_sequence_number')):
            with self.subTest(field=field,value=value):
                data=bytearray(original); both(data,16*BLOCK+offset,value,2); p.write_bytes(data)
                self.assertIn(f'descriptor=pvd field={field} result=0 le={value}',self.check_image(a))
        for lba,offset,field in ((16,881,'file_structure_version'),(16,6,'descriptor_version'),
                                 (17,7,'volume_flags'),(18,6,'descriptor_version')):
            with self.subTest(lba=lba,field=field):
                data=bytearray(original); data[lba*BLOCK+offset]=1 if offset==7 else 0; p.write_bytes(data)
                self.assertIn(f'field={field} result=-5',self.check_image(a,True))
        data=bytearray(original); data[18*BLOCK]=0; p.write_bytes(data)
        self.assertIn('descriptor=set field=terminator result=-5',self.check_image(a,True))
        data=bytearray(original); data[16*BLOCK+156]=0; p.write_bytes(data)
        self.assertIn('descriptor=pvd field=root_directory_record result=-5',self.check_image(a,True))

    def test_endian_halves(self):
        a=iso_fixtures.generate({'joliet':True},self.directory,310)
        p=Path(a['path']); original=p.read_bytes()
        for broken in ('big','little'):
            for lba in (16,17):
                for offset,width in ((80,4),(120,2),(124,2),(128,2)):
                    with self.subTest(broken=broken,lba=lba,offset=offset):
                        data=bytearray(original)
                        at=lba*BLOCK+offset+(width if broken=='big' else 0)
                        data[at:at+width]=b'\0'*width
                        # Volume set fields (120, 124) are informative and never refuse.
                        p.write_bytes(data); self.check_image(a,broken=='little' and offset not in (120,124))
            # Exercise root AND ordinary records in both hierarchies, including
            # bounds checks and the empty file's valid zero length.
            for offset,width in ((2,4),(10,4),(28,2)):
                with self.subTest(broken=broken,record_field=offset):
                    data=bytearray(original)
                    records=list(directory_records(original,16*BLOCK))+list(directory_records(original,17*BLOCK))
                    for record in records:
                        at=record+offset+(width if broken=='big' else 0)
                        data[at:at+width]=(0 if width==2 else (1<<(width*8))-1).to_bytes(width,'little')
                    p.write_bytes(data); self.check_image(a,broken=='little' and offset!=28)
        # Both values plausible: Linux's little-endian preference is stable.
        data=bytearray(original); data[16*BLOCK+84]^=1; data[17*BLOCK+84]^=1
        p.write_bytes(data); self.check_image(a)

    def test_volume_capacity_and_unused_path_tables(self):
        a=iso_fixtures.generate({'joliet':True},self.directory,320)
        p=Path(a['path']); original=p.read_bytes()
        for field in ('oversized','different_hierarchies','path_tables'):
            with self.subTest(field=field):
                data=bytearray(original)
                if field=='path_tables':
                    for lba in (16,17): data[lba*BLOCK+132:lba*BLOCK+156]=b'\0'*24
                else:
                    both(data,16*BLOCK+80,len(data)//BLOCK+64)
                    both(data,17*BLOCK+80,len(data)//BLOCK+(32 if field=='different_hierarchies' else 64))
                p.write_bytes(data); self.check_image(a)
        # A larger declared volume never authorizes a read past the medium.
        data=bytearray(original)
        for lba in (16,17):
            both(data,lba*BLOCK+80,len(data)//BLOCK+64)
            both(data,lba*BLOCK+158,len(data)//BLOCK+1)
        p.write_bytes(data)
        self.assertIn('descriptor=pvd field=extent result=-5',self.check_image(a,True))
        p.write_bytes(original)
        self.assertIn('field=device_sector_size result=-22',self.check_image(a,True,sector_size=512))
        self.assertIn('descriptor=pvd field=extent result=-5',self.check_image(a,True,sectors=17))

    def test_multisession_rock_ridge_joliet_and_boot(self):
        a=multisession_fixture(self.directory,330)
        data=Path(a['path']).read_bytes(); offsets=descriptors(data,a['session'])
        self.assertEqual([data[p] for p in offsets],[1,0,2,255])
        self.assertEqual(data[offsets[1]+7:offsets[1]+30].rstrip(b'\0'),b'EL TORITO SPECIFICATION')
        primary=offsets[0]
        self.assertEqual(int.from_bytes(data[primary+80:primary+84],'little'),len(data)//BLOCK-a['session'])
        self.assertLess(int.from_bytes(data[primary+80:primary+84],'little'),a['session'])
        # RR lives in directory System Use bytes, independently of the SVD.
        root=int.from_bytes(data[primary+158:primary+162],'little')*BLOCK
        self.assertIn(b'SP',data[root:root+data[root]])
        self.assertIn(b'RRIP_1991A',data)
        old_file=next(r for r in directory_records(data,primary)
                      if data[r+33:r+33+data[r+32]]==b'README.TXT;1')
        self.assertLess(int.from_bytes(data[old_file+2:old_file+6],'little'),a['session'])
        output=self.check_image(a)
        self.assertIn(f'session={a["session"]} lba={a["session"]+16} descriptor=pvd',output)
        self.assertTrue(any(e['path']=='/NEW.TXT' for e in a['entries']))
        # Some masters count the disc prefix too. Both conventions are
        # bounded by READ CAPACITY, with no double offset on file extents.
        for offset in offsets:
            if data[offset] in (1,2):
                modified=bytearray(Path(a['path']).read_bytes())
                both(modified,offset+80,len(data)//BLOCK)
                Path(a['path']).write_bytes(modified)
        self.check_image(a)

    def test_descriptor_order_repetition_unknowns_and_terminator(self):
        a=iso_fixtures.generate({'joliet':True},self.directory,340)
        p=Path(a['path']); original=p.read_bytes()
        headers={original[o]:original[o:o+BLOCK] for o in descriptors(original)}
        boot=bytearray(BLOCK); boot[0:7]=b'\0CD001\1'; boot[7:30]=b'EL TORITO SPECIFICATION'
        unknown=bytearray(boot); unknown[0]=42; unknown[6]=9
        foreign=bytearray(boot); foreign[1:6]=b'BEA01'
        enhanced=bytearray(headers[2]); enhanced[6]=2
        charset=bytearray(headers[2]); charset[90]=ord('X')
        variants=[([boot,headers[1],headers[2],headers[255]],False),
                  ([headers[2],headers[1],headers[255]],False),
                  ([headers[1],headers[1],headers[2],headers[255]],False),
                  ([foreign,unknown,headers[1],enhanced,charset,headers[2],headers[255]],False),
                  ([headers[1],headers[2],headers[255],headers[255],bytes(BLOCK)],False),
                  ([headers[2],headers[255]],True),
                  ([headers[255],headers[1],headers[2]],True),
                  ([headers[1],headers[2],boot],True)]
        for index,(records,refused) in enumerate(variants):
            with self.subTest(index=index):
                data=bytearray(original); session=len(data)//BLOCK
                data.extend(bytes((16+len(records))*BLOCK))
                for i,record in enumerate(records):
                    offset=(session+16+i)*BLOCK; data[offset:offset+BLOCK]=record
                    if data[offset] in (1,2): both(data,offset+80,len(data)//BLOCK)
                p.write_bytes(data)
                self.check_image({**a,'session':session},refused)

    def test_multi_extent_and_bounds(self):
        a=iso_fixtures.generate({'level':3},self.directory,400)
        p=Path(a['path']); data=bytearray(p.read_bytes())
        root=int.from_bytes(data[16*2048+158:16*2048+162],'little')*2048
        def records(base):
            offset=0
            while offset<2048 and data[base+offset]:
                length=data[base+offset]; yield base+offset
                offset+=length
        directory=next(r for r in records(root) if data[r+33:r+33+data[r+32]]==b'DATA')
        base=int.from_bytes(data[directory+2:directory+6],'little')*2048
        target=next(r for r in records(base) if data[r+33:r+33+data[r+32]]==b'ODD.BIN;1')
        length=data[target]; original=bytes(data[target:target+length]); extent=int.from_bytes(original[2:6],'little')
        # Insert a second contiguous record, preserving later siblings.
        following=bytes(data[target+length:base+2048-length])
        data[target+2*length:base+2048]=following
        data[target+length:target+2*length]=original
        def both(at,x):data[at:at+8]=x.to_bytes(4,'little')+x.to_bytes(4,'big')
        both(target+10,2048); data[target+25]|=128
        both(target+length+2,extent+1); both(target+length+10,4099-2048)
        p.write_bytes(data); self.check_image(a)
        both(target+length+2,len(data)//2048+20); p.write_bytes(data); self.check_image(a,True)

    def test_runner_slots_actions_and_manifest_refusal(self):
        with self.assertRaises(runner.res.Refusal):runner.fixture_disks({'fixtures':[{'generator':'mkfs.fat'}]*3+[{'generator':'xorriso'}]})
        fixture={'entries':[{'path':'/x','path_hex':'2f78','kind':'file','size':1,'sha256':'a'*64}]}
        records=[dict(case='entry',cycle=str(c),id='1',path_hex='2f78',kind='file') for c in (1,2)]
        records += [dict(case='file',cycle=str(c),id='1',size='1',sha256='a'*64) for c in (1,2)]
        iso_fixtures.verify_records(fixture,records)
        with self.assertRaises(ValueError):iso_fixtures.verify_records(fixture,records[:1])
        class Parser:terminal=False
        Parser.records=[{'event':'ARM','action':'media_reinsert','seq':'12'}]
        class Qmp:
            calls=[]
            def command(self,name,args):self.calls.append((name,args));return {}
        q=Qmp(); a=runner.Actions([{'type':'media','after':{'event':'ARM','action':'media_reinsert'},'operation':'change','device':'ciuki-optical','target':'fixture-2.iso'}])
        a.step(Parser,q,1); self.assertTrue(a.complete); self.assertEqual(q.calls[0][0],'blockdev-change-medium')
        self.assertEqual(runner.selector('f4:optical run=12345678','fw_cfg',True)['phase'],4)

class OpticalDrives(unittest.TestCase):
    """Campaign 12 (image e0af9210, optical-malformed-qemu-t23): with no FAT
    peer at IDE index 2 QEMU added its default empty CD-ROM there, the probe
    took that first packet device and reported no medium (-5, refused=0).
    Every optical case must present exactly the declared drive: an explicit
    ide-cd frontend on the secondary slave, which also clears QEMU's default
    CD-ROM (system/vl.c default_list), and no legacy if=ide CD-ROM."""
    def test_every_optical_case_declares_one_secondary_slave_ide_cd(self):
        suite=runner.load(ROOT/'tests/suites/f4-media.json')
        cases=[c for c in suite['cases'] if c['probe']=='optical']
        self.assertEqual(sorted({c['profile'] for c in cases if 'malformed' in c['id']}),['qemu-desktop-2002','qemu-t23'])
        for case in cases:
            with self.subTest(case=case['id']):
                profile=runner.load(ROOT/f"tests/profiles/{case['profile']}.json")
                args,request=runner.qemu_args('fixture',profile,case,'12345678',Path('overlay'),Path('bios'))
                self.assertEqual(request,'f4:optical run=12345678')
                iso=[i for i,f in enumerate(case['fixtures']) if f.get('generator')=='xorriso']
                self.assertEqual(len(iso),1)
                drives=[args[i+1] for i,a in enumerate(args) if a=='-drive']
                devices=[args[i+1] for i,a in enumerate(args) if a=='-device']
                self.assertEqual([d for d in drives if 'ciuki-optical' in d],
                                 [f'file=fixture-{iso[0]}.iso,format=raw,if=none,readonly=on,id=ciuki-optical'])
                self.assertEqual([d for d in devices if d.startswith('ide-')],['ide-cd,drive=ciuki-optical,bus=ide.1,unit=1'])
                self.assertFalse([d for d in drives if 'media=cdrom' in d or 'index=3' in d])
                # Legacy fixture disks keep IDE indices 1..2 next to the CD-ROM.
                disks=[d for d in drives if '.img,' in d]
                self.assertEqual(len(disks),len(case['fixtures'])-1)
                self.assertTrue(all(f'index={n},' in d for n,d in zip(range(1,3),disks)))

if __name__=='__main__':unittest.main()

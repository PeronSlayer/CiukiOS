"""f4-03 T0: production NTFS reads versus independent ntfs-3g oracles.

SPDX-License-Identifier: GPL-2.0-only
"""
import ctypes as C
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/"build/host/ntfs"
SPEC=importlib.util.spec_from_file_location("ntfs_fixtures",ROOT/"scripts/test/ntfs_fixtures.py")
FIX=importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(FIX)

class Node(C.Structure):
    _fields_=[("reference",C.c_uint64),("size",C.c_uint64),("directory",C.c_uint32)]
class Entry(C.Structure):
    _fields_=[("reference",C.c_uint64),("size",C.c_uint64),("directory",C.c_uint32),
              ("name_space",C.c_uint32),("name",C.c_char*1021)]
READ=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_uint32,C.c_void_p)
EMIT=C.CFUNCTYPE(C.c_int,C.c_void_p,C.POINTER(Entry))

class Volume:
    def __init__(self,lib,path,sector=512,sectors=None,fail_at=None,patch=None):
        self.lib=lib; self.path=path; self.fd=os.open(path,os.O_RDONLY)
        self.before=FIX.sha(path); self.sector=sector
        self.sectors=path.stat().st_size//sector if sectors is None else sectors
        self.boundary=min(self.sectors,FIX.get64(os.pread(self.fd,512,0),40))
        self.calls=0; self.outside=[]; self.fail_at=fail_at; self.patch=patch
        self.callback=READ(self.read)
        self.workspace=C.create_string_buffer(lib.ntfs_ro_workspace_size()+64)
        self.canary_start=lib.ntfs_ro_workspace_size()
        C.memset(C.addressof(self.workspace)+self.canary_start,0xa5,64)
        self.v=C.c_void_p(); self.reason=C.c_int()
        self.error=lib.ntfs_ro_mount(self.workspace,self.canary_start,self.callback,None,
                                   sector,self.sectors,C.byref(self.v),C.byref(self.reason))

    def read(self,_,lba,count,buf):
        self.calls+=1
        if not count or lba+count>self.boundary:
            self.outside.append((lba,count)); return -1
        if self.calls==self.fail_at: return -1
        data=os.pread(self.fd,count*self.sector,lba*self.sector)
        if self.patch: data=self.patch(lba,data)
        C.memmove(buf,data,len(data))
        return 0 if len(data)==count*self.sector else -1

    def finish(self,test):
        if self.v: self.lib.ntfs_ro_unmount(self.v)
        os.close(self.fd)
        test.assertEqual(self.outside,[])
        test.assertEqual(self.workspace.raw[self.canary_start:],bytes([0xa5])*64)
        test.assertEqual(FIX.sha(self.path),self.before,"read-only image changed")

    def root(self):
        n=Node(); err=self.lib.ntfs_ro_root(self.v,C.byref(n))
        if err: raise AssertionError(f"root {err}")
        return n

    def lookup(self,dir_,name):
        n=Node(); err=self.lib.ntfs_ro_lookup(self.v,C.byref(dir_),name.encode(),C.byref(n))
        return err,n

    def listing(self,dir_):
        rows=[]
        @EMIT
        def emit(_,e):
            rows.append({"inode":e.contents.reference & FIX.MASK,
                         "reference":e.contents.reference,"name":e.contents.name.decode(),
                         "namespace":e.contents.name_space,"directory":e.contents.directory})
            return 0
        err=self.lib.ntfs_ro_list(self.v,C.byref(dir_),emit,None)
        return err,rows

    def hash(self,n):
        h=hashlib.sha256(); off=0; buf=C.create_string_buffer(1048576); got=C.c_size_t()
        while off<n.size:
            err=self.lib.ntfs_ro_pread(self.v,C.byref(n),off,buf,len(buf),C.byref(got))
            if err: raise AssertionError(f"pread {err} at {off}")
            if not got.value: raise AssertionError("premature EOF")
            h.update(buf.raw[:got.value]); off+=got.value
        return {"sha256":h.hexdigest(),"size":off}

class NtfsReadOnly(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        absent=[t for t in (*FIX.TOOLS,"clang") if not shutil.which(t)]
        if absent: raise unittest.SkipTest("NTFS fixture tools missing: "+", ".join(absent))
        # Reuse immutable fixtures from this script/source state; never rebuild
        # the canonical kernel/apps/image from a test.
        script_hash=FIX.sha(ROOT/"scripts/test/ntfs_fixtures.py")
        marker=OUT/"generator.sha256"
        if not marker.exists() or marker.read_text().strip()!=script_hash:
            cls.manifest=FIX.build(OUT)
            marker.write_text(script_hash+"\n")
        else: cls.manifest=json.loads((OUT/"manifest.json").read_text())
        cls.fixtures={r["kind"]:r for r in cls.manifest["fixtures"]}
        for r in cls.fixtures.values():
            if FIX.sha(OUT/r["image"])!=r["image_sha256"]:
                raise AssertionError("fixture hash does not match manifest")
        flags=["-std=c17","-O1","-g","-Wall","-Wextra","-Werror","-I",str(ROOT/"src/kernel/include")]
        source=ROOT/"src/kernel/fs/ntfs_ro.c"
        subprocess.run(["clang",*flags,"-shared","-fPIC",str(source),"-o",str(OUT/"libntfs_ro.so")],check=True)
        subprocess.run(["clang",*flags,"-fsanitize=address,undefined",str(source),
                        str(ROOT/"tests/host/ntfs_ro_sanitize.c"),"-o",str(OUT/"sanitize")],check=True)
        cls.lib=C.CDLL(str(OUT/"libntfs_ro.so"))
        cls.lib.ntfs_ro_workspace_size.restype=C.c_size_t
        for name,args in {
            "mount":[C.c_void_p,C.c_size_t,READ,C.c_void_p,C.c_uint32,C.c_uint64,C.c_void_p,C.c_void_p],
            "root":[C.c_void_p,C.POINTER(Node)],"open":[C.c_void_p,C.c_uint64,C.POINTER(Node)],
            "lookup":[C.c_void_p,C.POINTER(Node),C.c_char_p,C.POINTER(Node)],
            "list":[C.c_void_p,C.POINTER(Node),EMIT,C.c_void_p],
            "pread":[C.c_void_p,C.POINTER(Node),C.c_uint64,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)],
            "unmount":[C.c_void_p],
        }.items(): getattr(cls.lib,"ntfs_ro_"+name).argtypes=args

    def volume(self,kind,**args):
        args.setdefault("sector",self.fixtures[kind].get("sector_size",512))
        v=Volume(self.lib,OUT/self.fixtures[kind]["image"],**args)
        self.addCleanup(v.finish,self)
        return v

    def verify_fixture(self,kind):
        fixture=self.fixtures[kind]; v=self.volume(kind)
        self.assertEqual((v.error,v.reason.value),(0,0))
        dirs={"/":v.root()}; references={}
        for path,expected in fixture["listings"].items():
            if path not in dirs:
                err,dirs[path]=v.lookup(dirs["/"],path[1:]); self.assertEqual(err,0)
            err,rows=v.listing(dirs[path]); self.assertEqual(err,0)
            actual=[{"inode":r["inode"],"name":r["name"]} for r in rows
                    if not r["name"].startswith("$") and r["namespace"]!=2]
            # ntfsls synthesizes '.' in subdirectories; this library emits
            # only actual on-disk index names, leaving synthetic VFS names
            # to the later integration directive.
            actual=[r for r in actual if r["name"] not in (".","..")]
            expected=[r for r in expected if r["name"] not in (".","..")]
            self.assertEqual(sorted(actual,key=lambda r:r["name"]),sorted(expected,key=lambda r:r["name"]))
            for row in rows: references[path.rstrip("/")+"/"+row["name"]]=row["reference"]
        for path,expected in fixture["files"].items():
            n=Node(); err=self.lib.ntfs_ro_open(v.v,references[path],C.byref(n))
            self.assertEqual(err,0,path)
            self.assertEqual(v.hash(n),expected,path)

    def test_clean_resident_unicode_alias_and_case(self):
        self.verify_fixture("clean")
        v=self.volume("clean"); root=v.root()
        err,a=v.lookup(root,"LONGFI~1.TXT"); self.assertEqual(err,0)
        err,b=v.lookup(root,"long FILENAME with SPACES.TXT"); self.assertEqual(err,0)
        self.assertEqual(a.reference,b.reference)
        err,n=v.lookup(root,"CAFFÈ-犬-😀.TXT"); self.assertEqual(err,0)
        self.assertEqual(v.hash(n),self.fixtures["clean"]["files"]["/caffè-犬-😀.txt"])
        for name,error in (("resident.txt:stream",-95),("bad/part",-22),("",-22),("no-file",-2)):
            self.assertEqual(v.lookup(root,name)[0],error)
        self.assertEqual(v.lookup(root,"x"*256)[0],-36)
        n=Node(); self.assertEqual(self.lib.ntfs_ro_lookup(v.v,C.byref(root),b"\xc0\x80",C.byref(n)),-84)
        self.assertEqual(self.lib.ntfs_ro_open(v.v,a.reference^(1<<48),C.byref(n)),-5)

    def test_large_index_allocation_bitmap_and_mft_growth(self): self.verify_fixture("large")
    def test_fragmentation_and_attribute_extensions(self):
        self.assertTrue(self.fixtures["fragmented"]["attribute_list"])
        self.verify_fixture("fragmented")
    def test_signed_relative_lcns(self): self.verify_fixture("negative")
    def test_sector_cluster_and_record_size_encodings(self):
        self.verify_fixture("sector-4096")
        self.verify_fixture("cluster-512")
    def test_mft_uses_own_fragmented_mapping(self): self.verify_fixture("mft-fragmented")
    def test_sparse_over_4g_and_initialized_tail(self):
        self.verify_fixture("sparse")
        v=self.volume("sparse"); err,n=v.lookup(v.root(),"huge.bin"); self.assertEqual(err,0)
        self.assertGreater(n.size,1<<32)
        buf=C.create_string_buffer(33); got=C.c_size_t(999)
        self.assertEqual(self.lib.ntfs_ro_pread(v.v,C.byref(n),(1<<32)+17,buf,33,C.byref(got)),0)
        self.assertEqual((got.value,buf.raw),(33,bytes(33)))
    def test_allocated_uninitialized_tail(self): self.verify_fixture("tail")

    def test_unsupported_content_is_never_exposed(self):
        for kind,name in (("compressed","compressed.bin"),("encrypted","allocated.bin"),("reparse","allocated.bin")):
            with self.subTest(kind=kind):
                v=self.volume(kind); self.assertEqual(v.error,0)
                self.assertEqual(v.lookup(v.root(),name)[0],-95)
                self.assertEqual(v.lookup(v.root(),"compressed.bin:other")[0],-95)

    def test_mount_refusals_and_corruption(self):
        for kind in ("recovery","hibernated","fixup","index","sequence","bitmap",
                     "log-recovery","log-hibernated","log-fixup","index-fixup","index-vcn"):
            with self.subTest(kind=kind):
                v=self.volume(kind)
                self.assertEqual(v.error,-5); self.assertEqual(v.reason.value,self.fixtures[kind]["reason"])
                self.assertFalse(v.v)
        v=self.volume("runlist"); self.assertEqual(v.error,0)
        self.assertEqual(v.lookup(v.root(),"allocated.bin")[0],-5)
        v=self.volume("attribute-list"); self.assertEqual(v.error,0)
        self.assertEqual(v.lookup(v.root(),"fragment.bin")[0],-5)
        v=self.volume("index-cycle"); self.assertEqual(v.error,0)
        err,n=v.lookup(v.root(),"many"); self.assertEqual(err,0)
        self.assertEqual(v.listing(n)[0],-5)

    def test_dirty_bit_requires_clean_log_and_no_hibernation(self):
        for kind in ("dirty","dirty-clean-log"):
            with self.subTest(kind=kind):
                v=self.volume(kind); self.assertEqual((v.error,v.reason.value),(0,3))
                error,node=v.lookup(v.root(),"resident.txt"); self.assertEqual(error,0)
                self.assertEqual(v.hash(node),self.fixtures["clean"]["files"]["/resident.txt"])
        for kind,reason in (("dirty-log-recovery",4),("dirty-hibernated",5)):
            with self.subTest(kind=kind):
                v=self.volume(kind); self.assertEqual((v.error,v.reason.value),(-5,reason)); self.assertFalse(v.v)

    def test_partition_bounds_and_short_io(self):
        v=self.volume("clean",sectors=1); self.assertEqual(v.error,-5)
        for step in (1,2,3,8,32):
            with self.subTest(step=step):
                v=self.volume("clean",fail_at=step); self.assertEqual(v.error,-5); self.assertFalse(v.v)
        v=self.volume("clean",sector=1024); self.assertEqual(v.error,-5)

    def test_bad_run_lengths_geometry_and_initialized_bounds(self):
        image=OUT/self.fixtures["clean"]["image"]
        d=FIX.Layout(image)
        inode=next(r["inode"] for r in self.fixtures["clean"]["listings"]["/"] if r["name"]=="allocated.bin")
        original,a=d.data_attr(inode)
        physical=d.physical(d.mapping,inode*d.record_size); d.close()
        for case in ("zero","gap","initialized","size","lcn","negative"):
            p=bytearray(original); run=a+FIX.get16(p,a+32)
            if case=="zero": p[run+1]=0
            if case=="gap": FIX.put64(p,a+24,FIX.get64(p,a+24)+1)
            if case=="initialized": FIX.put64(p,a+56,FIX.get64(p,a+48)+1)
            if case=="size": FIX.put64(p,a+48,(1<<64)-1)
            if case in ("lcn","negative"):
                n=p[run]>>4; start=run+1+(p[run]&15)
                p[start:start+n]=bytes([0xff])*n
                if case=="lcn": p[start+n-1]=0x7f
            raw=FIX.refix(p)
            def patch(lba,data,raw=raw):
                begin=lba*512; end=begin+len(data)
                lo=max(begin,physical); hi=min(end,physical+len(raw))
                if lo>=hi: return data
                modified=bytearray(data); modified[lo-begin:hi-begin]=raw[lo-physical:hi-physical]
                return bytes(modified)
            with self.subTest(case=case):
                v=self.volume("clean",patch=patch); self.assertEqual(v.error,0)
                self.assertEqual(v.lookup(v.root(),"allocated.bin")[0],-5)

    def test_fixture_generation_is_deterministic(self):
        repeat=ROOT/"build/host/ntfs-repeat"
        try:
            duplicate=FIX.build(repeat)
            self.assertEqual({r["kind"]:r["image_sha256"] for r in duplicate["fixtures"]},
                             {r["kind"]:r["image_sha256"] for r in self.manifest["fixtures"]})
        finally:
            if repeat.exists(): shutil.rmtree(repeat)

    def test_sanitized_walks_and_canaries(self):
        for fixture in self.fixtures.values():
            image=OUT/fixture["image"]; before=FIX.sha(image)
            result=subprocess.run([str(OUT/"sanitize"),str(image)],capture_output=True,text=True,
                                  env={**os.environ,"ASAN_OPTIONS":"detect_leaks=0"})
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(FIX.sha(image),before)

if __name__=="__main__": unittest.main()

#!/usr/bin/env python3
"""Build lawful, nonboot NTFS 3.1 fixtures and independent host-tool oracles.

SPDX-License-Identifier: GPL-2.0-only
No mounts, privileges or commercial data. Temporary files stay in build/.
"""
from __future__ import annotations
import argparse
import ctypes as C
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ("mkntfs", "ntfscp", "ntfsls", "ntfscat", "ntfstruncate", "ntfsinfo")
EPOCH = 116444736000000000  # NTFS timestamp for 1970-01-01
SERIAL = 0x4349554B494E5446
MASK = (1 << 48) - 1

def run(args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True,
                          env={**os.environ, "LC_ALL": "C.UTF-8", "TZ": "UTC"}).stdout

def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def cat_hash(image, name):
    h = hashlib.sha256(); size = 0
    with subprocess.Popen(["ntfscat", str(image), name], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE) as p:
        for chunk in iter(lambda: p.stdout.read(1024 * 1024), b""):
            h.update(chunk); size += len(chunk)
        error = p.stderr.read()
        if p.wait():
            raise RuntimeError(error.decode(errors="replace"))
    return {"size": size, "sha256": h.hexdigest()}

def listing(image, path="/", dos=False):
    args = ["ntfsls", "-i", "-p", path]
    if dos: args += ["-x"]
    rows = []
    for line in run([*args, image]).decode().splitlines():
        number, name = line.strip().split(maxsplit=1)
        rows.append({"inode": int(number), "name": name})
    return rows

def get16(p, off): return struct.unpack_from("<H", p, off)[0]
def get32(p, off): return struct.unpack_from("<I", p, off)[0]
def get64(p, off): return struct.unpack_from("<Q", p, off)[0]
def put16(p, off, n): struct.pack_into("<H", p, off, n)
def put32(p, off, n): struct.pack_into("<I", p, off, n)
def put64(p, off, n): struct.pack_into("<Q", p, off, n)

def unfix(raw):
    p = bytearray(raw); off, count = get16(p, 4), get16(p, 6)
    assert count == len(p) // 512 + 1
    for i in range(1, count):
        assert p[i*512-2:i*512] == p[off:off+2]
        p[i*512-2:i*512] = p[off+i*2:off+i*2+2]
    return p

def refix(p):
    p = bytearray(p); off, count = get16(p, 4), get16(p, 6)
    for i in range(1, count):
        p[off+i*2:off+i*2+2] = p[i*512-2:i*512]
        p[i*512-2:i*512] = p[off:off+2]
    return p

def attributes(p):
    off = get16(p, 20)
    while get32(p, off) != 0xffffffff:
        length = get32(p, off+4)
        assert length >= 24 and length % 8 == 0 and off+length <= get32(p, 24)
        yield off
        off += length

def attr_name(p, off):
    start = off+get16(p, off+10)
    return p[start:start+p[off+9]*2].decode("utf-16le")

def runs(p, off):
    pos = off+get16(p, off+32); lcn = 0; vcn = get64(p, off+16); out = []
    while p[pos]:
        ln, dn = p[pos] & 15, p[pos] >> 4; pos += 1
        length = int.from_bytes(p[pos:pos+ln], "little"); pos += ln
        if dn:
            lcn += int.from_bytes(p[pos:pos+dn], "little", signed=True)
            physical = lcn
        else: physical = None
        pos += dn; out.append((vcn, physical, length)); vcn += length
    return out

def replace_attr(p, off, value):
    """Resize one attribute within an existing fixture MFT record."""
    old=get32(p,off+4); used=get32(p,24); delta=len(value)-old
    assert used+delta <= len(p)
    tail=bytes(p[off+old:used])
    p[off:off+len(value)]=value
    p[off+len(value):used+delta]=tail
    if delta<0: p[used+delta:used]=bytes(-delta)
    put32(p,24,used+delta)

def mapping_attr(p, off, mapping):
    head=bytearray(p[off:off+get16(p,off+32)]); packed=bytearray(); previous=0
    for _,lcn,length in mapping:
        ln=max(1,(length.bit_length()+7)//8)
        delta=lcn-previous
        dn=next(n for n in range(1,9) if -(1<<(n*8-1))<=delta<(1<<(n*8-1)))
        packed.append(ln|(dn<<4)); packed+=length.to_bytes(ln,"little")
        packed+=delta.to_bytes(dn,"little",signed=True); previous=lcn
    value=head+packed+b"\0"; value+=bytes((-len(value))%8)
    put32(value,4,len(value)); replace_attr(p,off,value)

class Layout:
    """Fixture-only patching, independent of the production parser."""
    def __init__(self, image):
        self.image = Path(image)
        self.f = self.image.open("r+b")
        boot = self.f.read(512)
        self.sector = get16(boot, 11); self.cluster = self.sector*boot[13]
        self.mft = get64(boot, 48)*self.cluster
        self.mirror = get64(boot, 56)*self.cluster
        self.record_size = (1 << (256-boot[64])) if boot[64] > 127 else boot[64]*self.cluster
        self.f.seek(self.mft); m = unfix(self.f.read(self.record_size))
        data = next(a for a in attributes(m) if get32(m,a)==0x80 and not m[a+9])
        self.mapping = runs(m, data)
        self.records = get64(m,data+48)//self.record_size

    def physical(self, mapping, off):
        for vcn, lcn, length in mapping:
            if vcn*self.cluster <= off < (vcn+length)*self.cluster:
                assert lcn is not None
                return lcn*self.cluster + off-vcn*self.cluster
        raise ValueError("fixture mapping does not cover offset")

    def record(self, number):
        self.f.seek(self.physical(self.mapping,number*self.record_size))
        return unfix(self.f.read(self.record_size))

    def save_record(self, number, p):
        raw = refix(p)
        self.f.seek(self.physical(self.mapping,number*self.record_size)); self.f.write(raw)
        if number < 4:
            self.f.seek(self.mirror+number*self.record_size); self.f.write(raw)

    def data_attr(self, number, type_=0x80, name=""):
        p = self.record(number)
        return p, next(a for a in attributes(p) if get32(p,a)==type_ and attr_name(p,a)==name)

    def close(self): self.f.close()

def index_names(p, header):
    pos, end = header+get32(p,header), header+get32(p,header+4)
    while pos < end:
        length, flags = get16(p,pos+8), get16(p,pos+12)
        assert length >= 16
        if not flags & 2:
            for off in (8,16,24,32): put64(p,pos+16+off,EPOCH)
        pos += length

def normalize(image):
    """Normalize tool-created times and random serial without changing data."""
    d = Layout(image)
    for number in range(d.records):
        try: p = d.record(number)
        except AssertionError: continue
        if p[:4] != b"FILE": continue
        for a in attributes(p):
            type_ = get32(p,a)
            if not p[a+8]:
                value = a+get16(p,a+20)
                if type_==0x10:
                    for off in (0,8,16,24): put64(p,value+off,EPOCH)
                if type_==0x30:
                    for off in (8,16,24,32): put64(p,value+off,EPOCH)
                    # ntfs-3g leaves stale bytes between the end of the name
                    # and the 8-byte-aligned end of the attribute (one byte of
                    # MFT record 67 differed between generations, 2026-10-11).
                    end=value+0x42+2*p[value+0x40]; limit=a+get32(p,a+4)
                    if end<limit: p[end:limit]=bytes(limit-end)
                if type_==0x90 and attr_name(p,a)=="$I30": index_names(p,value+16)
            elif type_==0xa0 and attr_name(p,a)=="$I30":
                size = get64(p,a+48); mapping = runs(p,a)
                for off in range(0,size,4096):
                    physical = d.physical(mapping,off)
                    d.f.seek(physical); raw=d.f.read(4096)
                    if raw[:4] != b"INDX": continue
                    idx=unfix(raw); index_names(idx,24)
                    # Deleted/moved index entries leave old timestamps in
                    # unused slack. That slack is not directory contents.
                    used=24+get32(idx,28); idx[used:]=bytes(len(idx)-used)
                    d.f.seek(physical); d.f.write(refix(idx))
        # ntfs-3g may insert the DOS and Win32 FILE_NAME attributes in either
        # order on successive mounts in one process. Canonicalize that tied
        # attribute group by its unchanged ID; indexes/references stay intact.
        offsets=list(attributes(p))
        blocks=[bytes(p[a:a+get32(p,a+4)]) for a in offsets]
        names=iter(sorted((b for b in blocks if get32(b,0)==0x30),key=lambda b:get16(b,14)))
        blocks=[next(names) if get32(b,0)==0x30 else b for b in blocks]
        start=get16(p,20); packed=b"".join(blocks); p[start:start+len(packed)]=packed
        # Attribute moves/deletions also retain old names/times past used.
        # Keep metadata and data intact, normalize only unused record slack.
        used=get32(p,24); p[used:]=bytes(len(p)-used)
        d.save_record(number,p)
    for off in (0,Path(image).stat().st_size-d.sector):
        d.f.seek(off+72); d.f.write(struct.pack("<Q", SERIAL))
    d.close()

class HostNtfs:
    """Only fixture generation: ntfs-3g APIs missing a CLI (mkdir/unlink/alias).

    Installed libntfs-3g is invoked, never linked into Ciuki. The official dir.h
    signatures and ownership (delete/alias close both inodes) are respected.
    """
    def __init__(self, image):
        self.lib = C.CDLL(ctypes.util.find_library("ntfs-3g"), use_errno=True)
        signatures = {
            "ntfs_mount": ([C.c_char_p,C.c_ulong],C.c_void_p),
            "ntfs_umount": ([C.c_void_p,C.c_int],C.c_int),
            "ntfs_inode_open": ([C.c_void_p,C.c_uint64],C.c_void_p),
            "ntfs_inode_close": ([C.c_void_p],C.c_int),
            "ntfs_pathname_to_inode": ([C.c_void_p,C.c_void_p,C.c_char_p],C.c_void_p),
            "ntfs_create": ([C.c_void_p,C.c_uint32,C.c_void_p,C.c_uint8,C.c_uint32],C.c_void_p),
            "ntfs_delete": ([C.c_void_p,C.c_char_p,C.c_void_p,C.c_void_p,C.c_void_p,C.c_uint8],C.c_int),
            "ntfs_set_ntfs_dos_name": ([C.c_void_p,C.c_void_p,C.c_char_p,C.c_size_t,C.c_int],C.c_int),
            "ntfs_attr_open": ([C.c_void_p,C.c_uint32,C.c_void_p,C.c_uint32],C.c_void_p),
            "ntfs_attr_pwrite": ([C.c_void_p,C.c_int64,C.c_int64,C.c_void_p],C.c_int64),
            "ntfs_attr_close": ([C.c_void_p],None),
        }
        for name,(args,result) in signatures.items():
            f=getattr(self.lib,name); f.argtypes=args; f.restype=result
        self.volume=self.lib.ntfs_mount(os.fsencode(image),0)
        if not self.volume: raise OSError(C.get_errno(), "fixture mount")

    def inode(self,path):
        p=self.lib.ntfs_pathname_to_inode(self.volume,None,path.encode())
        if not p: raise OSError(C.get_errno(),path)
        return p

    def directory(self,name,where="/"):
        parent=self.inode(where); utf=C.create_string_buffer(name.encode("utf-16le"))
        p=self.lib.ntfs_create(parent,0,utf,len(name),0o040000)
        if not p: raise OSError(C.get_errno(),name)
        assert self.lib.ntfs_inode_close(p)==0
        assert self.lib.ntfs_inode_close(parent)==0

    def delete(self,name):
        parent=self.inode("/"); p=self.inode("/"+name)
        utf=C.create_string_buffer(name.encode("utf-16le"))
        assert self.lib.ntfs_delete(self.volume,("/"+name).encode(),p,parent,utf,len(name))==0

    def alias(self,name,alias):
        assert self.lib.ntfs_set_ntfs_dos_name(self.inode("/"+name),self.inode("/"),
                                              alias.encode(),len(alias),0)==0

    def fragment(self):
        inodes=[self.inode("/fragment.bin"),self.inode("/blocker.bin")]
        attrs=[self.lib.ntfs_attr_open(n,0x80,None,0) for n in inodes]
        assert all(attrs)
        for i in range(240):
            for k,a in enumerate(attrs):
                data=bytes([(i+k)%251])*4096
                assert self.lib.ntfs_attr_pwrite(a,i*4096,4096,C.c_char_p(data))==4096
        for a in attrs: self.lib.ntfs_attr_close(a)
        for n in inodes: assert self.lib.ntfs_inode_close(n)==0
        self.delete("blocker.bin")

    def close(self): assert self.lib.ntfs_umount(self.volume,0)==0

def disk_image(volume,out,start=2048):
    """MBR disk holding an unmodified copy of one volume image (f4-04).

    One primary partition of type 07h starts at LBA ``start`` (1 MiB, the usual
    alignment); the volume bytes, including the NTFS boot sector, are copied
    verbatim and sparsely, so the independent ntfs-3g oracle hashes still
    describe the partition. No random disk signature: the result is
    deterministic. The volume's own hidden-sectors field stays as generated
    (0): the driver does not use it. Returns the image path.
    """
    volume=Path(volume); out=Path(out); size=volume.stat().st_size
    if size%512 or start<1: raise ValueError("volume must be whole sectors, partition after the MBR")
    sectors=size//512
    mbr=bytearray(512)
    mbr[446+4]=0x07                        # NTFS/HPFS/exFAT partition type
    mbr[446+1:446+4]=bytes((0xfe,0xff,0xff)) # CHS first sector: beyond CHS, LBA only
    mbr[446+5:446+8]=bytes((0xfe,0xff,0xff))
    struct.pack_into("<II",mbr,446+8,start,sectors)
    mbr[510:512]=b"\x55\xaa"
    with volume.open("rb") as src, out.open("wb") as dst:
        dst.truncate((start+sectors)*512)
        dst.write(mbr)
        offset=0
        while offset<size:
            chunk=src.read(1024*1024)
            if not chunk: break
            if chunk.count(0)!=len(chunk):   # keep holes sparse
                dst.seek(start*512+offset); dst.write(chunk)
            offset+=len(chunk)
    return out

def set_attributes(image,flags_by_name):
    """f4-04 fixture: DOS attribute bits (RO 1, hidden 2, system 4) set where
    Windows keeps them: the FILE_NAME key of the root index entries (resident
    root and every INDEX_ALLOCATION block) and the file's own
    $STANDARD_INFORMATION and $FILE_NAME. Fixture-only patching, as Layout."""
    d=Layout(image); numbers={}
    def patch(buf,header):
        pos,end=header+get32(buf,header),header+get32(buf,header+4)
        while pos<end:
            length,flags=get16(buf,pos+8),get16(buf,pos+12)
            if flags&2: break
            key=pos+16; name=bytes(buf[key+66:key+66+buf[key+64]*2]).decode("utf-16le")
            if name in flags_by_name:
                put32(buf,key+56,get32(buf,key+56)|flags_by_name[name]); numbers[name]=get64(buf,pos)&MASK
            pos+=length
    p=d.record(5)
    for a in attributes(p):
        if get32(p,a)==0x90 and attr_name(p,a)=="$I30" and not p[a+8]: patch(p,a+get16(p,a+20)+16)
        if get32(p,a)==0xa0 and attr_name(p,a)=="$I30":
            mapping=runs(p,a)
            for off in range(0,get64(p,a+48),4096):
                physical=d.physical(mapping,off); d.f.seek(physical); raw=d.f.read(4096)
                if raw[:4]!=b"INDX": continue
                idx=unfix(raw); patch(idx,24); d.f.seek(physical); d.f.write(refix(idx))
    d.save_record(5,p)
    assert set(numbers)==set(flags_by_name), "attribute fixture names not found in the root index"
    for name,number in numbers.items():
        q=d.record(number)
        for a in attributes(q):
            value=a+get16(q,a+20) if not q[a+8] else 0
            if value and get32(q,a)==0x10: put32(q,value+32,get32(q,value+32)|flags_by_name[name])
            if value and get32(q,a)==0x30: put32(q,value+56,get32(q,value+56)|flags_by_name[name])
        d.save_record(number,q)
    d.close()

def copy_image(src,dst):
    run(["cp","--reflink=auto","--sparse=always",src,dst])

def format_image(path, compressed=False, sector=512, cluster=4096):
    with path.open("wb") as f: f.truncate(32*1024*1024)
    run(["mkntfs","-F","-q","-T","-s",str(sector),"-c",str(cluster),
         "-p","0","-H","16","-S","63",*(["-C"] if compressed else []),path])

def populate(image,temp,name,data):
    temp.write_bytes(data); os.utime(temp,(0,0))
    run(["ntfscp","-q","-t",image,temp,name])

def manifest(image, tools, paths=("/",), **extra):
    lists={path:listing(image,path) for path in paths}
    files={}
    for path,rows in lists.items():
        for row in rows:
            full=path.rstrip("/")+"/"+row["name"]
            if row["name"] not in (".","..") and full not in paths:
                files[full]=cat_hash(image,full)
    return {"image":image.name,"image_sha256":sha(image),"image_bytes":image.stat().st_size,
            "tools":tools,"listings":lists,"files":files,**extra}

def build(out, entries=2048, only=None):
    """All fixture kinds, or only the named ones (with the kinds they derive from)."""
    out=Path(out).resolve()
    if not out.is_relative_to(ROOT/"build") or out==ROOT/"build":
        raise ValueError("fixtures need a subdirectory of this worktree's build/")
    if out.exists(): shutil.rmtree(out)
    out.mkdir(parents=True)
    tools={}
    for name in TOOLS:
        # ntfsls and some older ntfsprogs exit 1 even for --version.
        version=subprocess.run([name,"-V"],capture_output=True)
        text=(version.stdout+version.stderr).decode(errors="replace").strip()
        if name not in text: raise RuntimeError(f"cannot identify {name}")
        tools[name]={"version":text,"executable_sha256":sha(shutil.which(name))}
    libpath=Path("/usr/lib/libntfs-3g.so").resolve()
    if libpath.exists(): tools["libntfs-3g"]={"path":str(libpath),"sha256":sha(libpath)}
    temp=out/"payload.tmp"; images=[]
    deps={"attribute-list":{"fragmented"},"index-cycle":{"large"}}
    wanted=None if only is None else set(only).union(*(deps.get(k,set()) for k in only))
    def want(*kinds): return wanted is None or any(k in wanted for k in kinds)
    base=out/"clean.img"; format_image(base)
    payloads={"/resident.txt":b"Ciuki NTFS resident\n", "/empty.bin":b"",
              "/allocated.bin":bytes(range(256))*257,
              "/Long filename with spaces.txt":b"one node, two names\n",
              "/caffè-犬-😀.txt":"UTF-8 from UTF-16\n".encode(),
              "/"+"n"*240+".txt":b"long component\n"}
    for name,data in payloads.items(): populate(base,temp,name,data)
    host=HostNtfs(base); host.alias("Long filename with spaces.txt","LONGFI~1.TXT"); host.close()
    normalize(base)
    images.append(manifest(base,tools,dos_listing=listing(base,dos=True),kind="clean"))

    for kind,sector,cluster in (("sector-4096",4096,16384),("cluster-512",512,512)):
        if not want(kind): continue
        image=out/(kind+".img"); format_image(image,sector=sector,cluster=cluster)
        for name,data in payloads.items(): populate(image,temp,name,data)
        normalize(image)
        images.append(manifest(image,tools,kind=kind,sector_size=sector))

    if want("large"):
        large=out/"large.img"; format_image(large)
        host=HostNtfs(large); host.directory("many"); host.close()
        for i in range(entries): populate(large,temp,f"/many/entry-{i:05d}.txt",f"entry {i:05d}\n".encode())
        normalize(large)
        images.append(manifest(large,tools,paths=("/","/many"),kind="large",entries=entries))

    if want("fragmented"):
        fragmented=out/"fragmented.img"; format_image(fragmented)
        for name in ("/fragment.bin","/blocker.bin"): populate(fragmented,temp,name,b"")
        host=HostNtfs(fragmented); host.fragment(); host.close(); normalize(fragmented)
        frag_inode=listing(fragmented)[0]["inode"]
        info=run(["ntfsinfo","-f","-i",frag_inode,fragmented]).decode()
        d=Layout(fragmented); p=d.record(frag_inode)
        has_list=any(get32(p,a)==0x20 for a in attributes(p)); d.close()
        if not has_list: raise RuntimeError("fragment workload did not produce ATTRIBUTE_LIST")
        images.append(manifest(fragmented,tools,kind="fragmented",ntfsinfo=info,attribute_list=True))

    if want("negative"):
        # Reorder allocated clusters within the same allocation (no cross-links,
        # new allocation or content changes) to require a signed negative LCN.
        negative=out/"negative.img"; copy_image(base,negative)
        d=Layout(negative)
        inode=next(r["inode"] for r in listing(base) if r["name"]=="allocated.bin")
        p,a=d.data_attr(inode); original=runs(p,a)
        assert len(original)==1
        _,lcn,length=original[0]; split=length//2
        d.f.seek(lcn*d.cluster); content=d.f.read(length*d.cluster)
        first=length-split
        d.f.seek((lcn+split)*d.cluster); d.f.write(content[:first*d.cluster])
        d.f.seek(lcn*d.cluster); d.f.write(content[first*d.cluster:])
        mapping_attr(p,a,[(0,lcn+split,first),(first,lcn,split)]); d.save_record(inode,p); d.close()
        images.append(manifest(negative,tools,kind="negative"))

    if want("mft-fragmented"):
        # Fragment $MFT itself by rotating its already-owned clusters. Future
        # record reads must follow this map, rather than assume boot-LCN+offset.
        mft_fragmented=out/"mft-fragmented.img"; copy_image(base,mft_fragmented)
        d=Layout(mft_fragmented); assert len(d.mapping)==1
        _,lcn,length=d.mapping[0]; cut=4
        mapping=[(0,lcn,length-cut),(length-cut,lcn+length-cut+1,cut-1),(length-1,lcn+length-cut,1)]
        # The first segment stays in place for bootstrap. Rotate the final four
        # clusters, updating record 0 and its mirror with the new run mapping.
        d.f.seek((lcn+length-cut)*d.cluster); content=d.f.read(cut*d.cluster)
        d.f.seek((lcn+length-cut+1)*d.cluster); d.f.write(content[:(cut-1)*d.cluster])
        d.f.seek((lcn+length-cut)*d.cluster); d.f.write(content[(cut-1)*d.cluster:])
        p,a=d.data_attr(0); mapping_attr(p,a,mapping); d.save_record(0,p); d.close()
        images.append(manifest(mft_fragmented,tools,kind="mft-fragmented"))

    if want("compressed"):
        compressed=out/"compressed.img"; format_image(compressed,True)
        populate(compressed,temp,"/compressed.bin",b"compress me\n"*10000); normalize(compressed)
        images.append(manifest(compressed,tools,kind="compressed",file_error=-95))

    if want("sparse"):
        sparse=out/"sparse.img"; copy_image(base,sparse)
        populate(sparse,temp,"/huge.bin",b"initialized prefix\n"*300)
        inode=next(r["inode"] for r in listing(sparse) if r["name"]=="huge.bin")
        run(["ntfstruncate","-q",sparse,inode,str((1<<32)+65537)])
        normalize(sparse)
        images.append(manifest(sparse,tools,kind="sparse",large_inode=inode))

    if want("names"):
        # f4-04: names the VFS folds or cannot hold, and DOS attribute bits.
        names=out/"names.img"; format_image(names)
        host=HostNtfs(names); host.directory("Sub dir"); host.directory("deeper","/Sub dir"); host.close()
        for name,data in {"/\u1f80.txt":b"greek collation\n","/\U0001f600 smile.txt":b"alias names the file\n",
                          "/\U0001f600 plain.txt":b"omitted from the view\n","/plain ascii.txt":b"ascii\n",
                          "/Sub dir/inner.txt":b"inside a directory\n","/Sub dir/Mixed Case.TXT":b"mixed case\n",
                          "/Sub dir/deeper/leaf.txt":b"two levels down\n"}.items():
            populate(names,temp,name,data)
        host=HostNtfs(names); host.alias("\U0001f600 smile.txt","SMILE~1.TXT"); host.close()
        normalize(names)
        images.append(manifest(names,tools,paths=("/","/Sub dir","/Sub dir/deeper"),kind="names",
                               dos_listing=listing(names,dos=True)))

    if want("attributes"):
        attributes_img=out/"attributes.img"; copy_image(base,attributes_img)
        set_attributes(attributes_img,{"resident.txt":2,"empty.bin":4,"allocated.bin":1})
        images.append(manifest(attributes_img,tools,kind="attributes",
                               attributes={"resident.txt":2,"empty.bin":4,"allocated.bin":1}))

    # Mutations are fixture inputs, never driver repair. Reflinks/sparse copies
    # are allowed for data fixtures; canonical system images are never copied.
    for kind in ("dirty","recovery","hibernated","fixup","runlist","index","sequence",
                 "encrypted","reparse","tail","bitmap","index-cycle","attribute-list",
                 "log-recovery","log-hibernated","log-fixup","index-fixup","index-vcn"):
        if not want(kind): continue
        image=out/(kind+".img")
        source=fragmented if kind=="attribute-list" else large if kind=="index-cycle" else base
        copy_image(source,image)
        if kind=="hibernated":
            populate(image,temp,"/hiberfil.sys",b"hibr"+bytes(4092)); normalize(image)
        else:
            d=Layout(image)
            if kind in ("dirty","recovery"):
                p,a=d.data_attr(3,0x70); put16(p,a+get16(p,a+20)+10,1 if kind=="dirty" else 4)
                d.save_record(3,p)
            elif kind.startswith("log-"):
                p,a=d.data_attr(2); mapping=runs(p,a)
                for page in range(2):
                    idx=bytearray(4096); idx[:4]=b"RSTR"
                    put16(idx,4,32); put16(idx,6,9); put16(idx,32,1)
                    put32(idx,16,4096); put32(idx,20,4096); put16(idx,24,64)
                    put16(idx,26,0); put16(idx,28,2 if kind=="log-hibernated" else 1)
                    put16(idx,64+8,1); put16(idx,64+12,0); put16(idx,64+20,64)
                    raw=refix(idx)
                    if kind=="log-fixup": raw[510:512]=b"xx"
                    d.f.seek(d.physical(mapping,page*4096)); d.f.write(raw)
            elif kind=="attribute-list":
                p,a=d.data_attr(64,0x20)
                if p[a+8]:
                    off=d.physical(runs(p,a),0); d.f.seek(off); le=bytearray(d.f.read(32))
                    put16(le,4,0); d.f.seek(off); d.f.write(le)
                else: put16(p,a+get16(p,a+20)+4,0); d.save_record(64,p)
            elif kind=="bitmap":
                p,a=d.data_attr(5,0xb0,"$I30"); assert not p[a+8]
                p[a+get16(p,a+20)]=0; d.save_record(5,p)
            elif kind=="index-cycle":
                many=next(r["inode"] for r in listing(large) if r["name"]=="many")
                p,a=d.data_attr(many,0x90,"$I30"); value=a+get16(p,a+20)
                entry=value+16+get32(p,value+16)
                child=get64(p,entry+get16(p,entry+8)-8)
                p,a=d.data_attr(many,0xa0,"$I30"); off=d.physical(runs(p,a),child*d.cluster)
                d.f.seek(off); idx=unfix(d.f.read(4096)); entry=24+get32(idx,24)
                # Insert a subnode pointer into a leaf and mark it internal,
                # preserving the key. It points back to this same INDX VCN.
                length=get16(idx,entry+8); end=24+get32(idx,28)
                idx[entry+length+8:end+8]=idx[entry+length:end]
                put16(idx,entry+8,length+8); put16(idx,entry+12,1)
                put64(idx,entry+length,child); put32(idx,28,get32(idx,28)+8); put32(idx,36,1)
                d.f.seek(off); d.f.write(refix(idx))
            elif kind=="fixup":
                d.f.seek(d.mft+510); d.f.write(b"xx")
            elif kind in ("runlist","encrypted","reparse","tail"):
                inode=next(r["inode"] for r in listing(base) if r["name"]=="allocated.bin")
                p,a=d.data_attr(inode)
                if kind=="runlist": p[a+get16(p,a+32)]=0x99
                if kind=="encrypted": put16(p,a+12,0x4000)
                if kind=="reparse":
                    si=next(x for x in attributes(p) if get32(p,x)==0x10)
                    put32(p,si+get16(p,si+20)+32,0x400)
                if kind=="tail": put64(p,a+56,13)
                d.save_record(inode,p)
            else:
                p,a=d.data_attr(5,0xa0,"$I30"); mapping=runs(p,a)
                off=d.physical(mapping,0); d.f.seek(off); idx=unfix(d.f.read(4096))
                entry=24+get32(idx,24)
                if kind=="index": put16(idx,entry+8,8)
                if kind=="sequence": put64(idx,entry,get64(idx,entry) ^ (1<<48))
                if kind=="index-vcn": put64(idx,16,1)
                raw=refix(idx)
                if kind=="index-fixup": raw[510:512]=b"xx"
                d.f.seek(off); d.f.write(raw)
            d.close()
        if kind=="tail": images.append(manifest(image,tools,kind=kind))
        else:
            oracle=next(row for row in images if row["image"]==source.name)
            expected_files=dict(oracle["files"])
            if kind=="hibernated": expected_files["/hiberfil.sys"]=cat_hash(image,"/hiberfil.sys")
            images.append({"image":image.name,"image_sha256":sha(image),
                           "image_bytes":image.stat().st_size,"tools":tools,"kind":kind,
                           "source_files":expected_files,"source_listings":oracle["listings"],
                           "hash_source":"independent ntfscat on the source before structural corruption; data extents unchanged",
                           "error":-95 if kind in ("encrypted","reparse") else -5,
                           "reason":{"dirty":3,"recovery":4,"hibernated":5,
                                     "log-recovery":4,"log-hibernated":5}.get(kind,1)})
    temp.unlink()
    images=[r for r in images if only is None or r["kind"] in only]
    result={"schema":1,"serial":SERIAL,"timestamp":EPOCH,"fixtures":images}
    (out/"manifest.json").write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n")
    return result

# ---- f4-04: what the VFS may show, and QEMU fixture/evidence support --------
FORBIDDEN=set('"*/:<>?\\|')
# Statuses the VFS must return for a listed file: EOPNOTSUPP (-95) for
# compressed/encrypted/reparse content, EIO (-5) for damaged metadata.
EXPECT_STATUS={"compressed":{"/compressed.bin":-95},"encrypted":{"/allocated.bin":-95},
               "reparse":{"/allocated.bin":-95},"runlist":{"/allocated.bin":-5},
               "attribute-list":{"/fragment.bin":-5}}
# Mount refusal: manifest reason code -> (diagnostic, error)
REFUSAL={3:("ntfs_dirty",-117),4:("ntfs_recovery_required",-117),5:("ntfs_hibernated",-117),1:("ntfs_malformed",-5)}
QEMU_KINDS=("clean","fragmented","negative","mft-fragmented","tail","names","attributes","compressed",
            "encrypted","reparse","runlist","attribute-list","sparse","dirty","recovery","hibernated",
            "fixup","index","sequence","bitmap","log-recovery","log-hibernated","log-fixup",
            "index-fixup","index-vcn")

def representable(name):
    """The VFS holds BMP code points, no controls or reserved characters and no
    trailing dot or space (path_validate_name)."""
    return bool(name) and name[-1] not in " ." and all(32<=ord(c)<=0xffff and ord(c)!=127 and c not in FORBIDDEN for c in name)

def short_alias(name):
    base,_,ext=name.partition(".")
    return bool(name) and name==name.upper() and bool(base) and len(base)<=8 and len(ext)<=3 and "." not in ext \
        and all(c.isascii() and (c.isalnum() or c in "_$-~") for c in base+ext)

def vfs_view(row):
    """The oracle's tree as the VFS may show it: path -> oracle hash record,
    "directory", or None when only a listing exists, plus the names omitted because
    the VFS cannot hold them and no valid DOS alias names the node."""
    listings=row.get("listings",row.get("source_listings",{})); files=row.get("files",row.get("source_files",{}))
    dos={r["inode"]:r["name"] for r in row.get("dos_listing",[])}
    view={}; omitted=0
    for directory,rows in listings.items():
        for entry in rows:
            if entry["name"] in (".",".."): continue
            shown=entry["name"]
            if not representable(shown):
                alias=dos.get(entry["inode"])
                if alias and alias!=shown and short_alias(alias): shown=alias
                else: omitted+=1; continue
            full=directory.rstrip("/")+"/"+entry["name"]
            view[directory.rstrip("/")+"/"+shown]="directory" if full in listings else files.get(full)
    return view,omitted

def qemu_fixture(spec,directory,index):
    """One declared fixture for a QEMU case: the named kind, wrapped in an MBR
    disk (partition type 07h at LBA 2048). The oracle row stays in the manifest."""
    kind=spec.get("kind")
    if kind not in QEMU_KINDS or spec.get("seed")!=1: raise ValueError("unsupported NTFS fixture kind/seed")
    work=Path(directory)/f"ntfs-fixture-{index}"
    row=build(work,only={kind})["fixtures"][0]
    image=Path(directory)/f"fixture-{index}.img"
    disk_image(work/row["image"],image)
    shutil.rmtree(work)
    return {"path":str(image),"size":image.stat().st_size,"sha256":sha(image),"volume_sha256":row["image_sha256"],
            "kind":kind,"partition_start":2048,"oracle":row}

def verify_records(item,records):
    """Compare the probe's records with the independent oracle (fail closed)."""
    kind=item["kind"]; row=item["oracle"]
    device=[r for r in records if r.get("case")=="device"]
    if len(device)!=1: raise ValueError("NTFS probe needs exactly one device record")
    if any(r.get("case")=="walkerr" for r in records): raise ValueError("NTFS enumeration failed")
    if "reason" in row:
        diagnostic,error=REFUSAL[row["reason"]]
        refusal=[r for r in records if r.get("case")=="refusal"]
        if device[0].get("reason")!=diagnostic or int(device[0].get("result","0"))!=error:
            raise ValueError("NTFS device record differs from the manifest reason")
        if len(refusal)!=1 or refusal[0].get("reason")!=diagnostic or int(refusal[0].get("result","0"))!=error \
                or refusal[0].get("letter_absent")!="1" or refusal[0].get("writes")!="0":
            raise ValueError("NTFS refusal differs from the manifest reason")
        if any(r.get("case") in ("entry","file") for r in records): raise ValueError("refused NTFS volume exposed entries")
        return
    if device[0].get("result")!="0" or device[0].get("reason")!="ntfs": raise ValueError("NTFS fixture did not mount")
    view,omitted=vfs_view(row); statuses=EXPECT_STATUS.get(kind,{})
    for cycle in ("1","2"):
        entries={r["path_sha256"]:r for r in records if r.get("case")=="entry" and r.get("cycle")==cycle}
        if len(entries)!=len([r for r in records if r.get("case")=="entry" and r.get("cycle")==cycle]):
            raise ValueError("duplicate NTFS entry identity")
        expected={hashlib.sha256(path.encode()).hexdigest():path for path in view}
        if set(entries)!=set(expected): raise ValueError("NTFS listing differs from the independent manifest, cycle "+cycle)
        for digest,path in expected.items():
            entry=entries[digest]; oracle=view[path]
            if entry.get("kind")!=("directory" if oracle=="directory" else "file") or int(entry["plen"])!=len(path.encode()):
                raise ValueError("NTFS entry type/name length differs: "+path)
            if oracle=="directory":
                if entry["status"]!="0": raise ValueError("NTFS directory refused: "+path)
                continue
            if int(entry["status"])!=statuses.get(path,0): raise ValueError("NTFS entry status differs: "+path)
            if entry["status"]!="0":
                state=[r for r in records if r.get("case")=="status" and r.get("cycle")==cycle and r.get("id")==entry["id"]]
                if len(state)!=1 or state[0]["open"]!=entry["status"] or state[0]["stat"]!=entry["status"]:
                    raise ValueError("NTFS unsupported file was not refused with its status: "+path)
                continue
            large=[r for r in records if r.get("case")=="large" and r.get("cycle")==cycle and r.get("id")==entry["id"]]
            hashes=[r for r in records if r.get("case")=="file" and r.get("cycle")==cycle and r.get("id")==entry["id"]]
            if oracle["size"]>0xffffffff:
                if len(large)!=1 or len(hashes) or int(large[0]["size"])!=oracle["size"]: raise ValueError("NTFS large file differs: "+path)
                continue
            if len(hashes)!=1 or len(large) or hashes[0].get("sha256")!=oracle["sha256"] or int(hashes[0]["size"])!=oracle["size"]:
                raise ValueError("NTFS file hash/size differs from the independent manifest: "+path)
        names=[r for r in records if r.get("case")=="names" and r.get("cycle")==cycle]
        if len(names)!=1 or int(names[0]["omitted"])!=omitted: raise ValueError("NTFS omitted-name count differs, cycle "+cycle)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out",type=Path,default=ROOT/"build/host/ntfs")
    p.add_argument("--entries",type=int,default=2048)
    args=p.parse_args()
    if not 1000 <= args.entries <= 10000: p.error("--entries must be 1000..10000")
    build(args.out,args.entries)
    print(f"NTFS fixtures: {args.out}/manifest.json")

if __name__=="__main__": main()

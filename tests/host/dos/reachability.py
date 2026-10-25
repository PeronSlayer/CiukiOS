"""Bounded LZEXE load-module audit; static reachability is never an execution trace.

Unchanged pinned archives are read through the canonical reader. Decompressed
bytes remain in memory, never become game/release payloads. Independent reader
of LZEXE stream semantics (UNLZEXE upstream cited in f3-51 validation).
SPDX-License-Identifier: GPL-2.0-only
"""
import hashlib
import importlib.util
import json
import re
import struct
import subprocess
from functools import lru_cache
from inventory import CORPUS, ROOT, sites


def lzexe(data, limit=2*1024*1024):
    if len(data)<32 or data[:2]!=b"MZ" or data[28:32] not in (b"LZ91",b"LZ09"):
        raise ValueError("not a supported LZEXE MZ")
    header=struct.unpack_from("<16H",data)
    info_at=(header[4]+header[11])*16
    if info_at+16>len(data): raise ValueError("truncated LZEXE descriptor")
    info=struct.unpack_from("<8H",data,info_at)
    start=(header[11]-info[4]+header[4])*16
    if start<32 or start>=info_at: raise ValueError("invalid LZEXE stream extent")
    cursor=start
    def byte():
        nonlocal cursor
        if cursor>=info_at: raise ValueError("truncated LZEXE stream")
        value=data[cursor];cursor+=1;return value
    def word(): return byte() | byte()<<8
    control=word(); left=16
    def bit():
        nonlocal control,left
        value=control&1;left-=1
        if left==0: control=word();left=16
        else: control>>=1
        return value
    out=bytearray()
    while True:
        if len(out)>=limit: raise ValueError("LZEXE output budget exhausted")
        if bit(): out.append(byte());continue
        if not bit():
            length=(bit()<<1)|bit();length+=2; distance=byte()-256
        else:
            low=byte(); packed=byte(); distance=(low|((packed&0xf8)<<5)|0xe000)-65536
            length=(packed&7)+2
            if length==2:
                length=byte()
                if length==0: break
                if length==1: continue
                length+=1
        if distance>=0 or -distance>len(out) or len(out)+length>limit:
            raise ValueError("invalid LZEXE back-reference")
        for _ in range(length): out.append(out[len(out)+distance])
    entry=info[1]*16+info[0]
    if not 0<=entry<len(out): raise ValueError("LZEXE original entry outside load module")
    return bytes(out),entry


def reachable(data, entry, bits=16, limit=200000):
    """Decode from each reached basic-block boundary, never a linear sweep.

    Decode windows are bounded and discarded after a branch. Backward targets
    are decoded at their actual address; conflicting instruction starts remain
    unresolved. Absolute/far/indirect edges need relocation/runtime evidence.
    """
    if bits not in (16,32) or not 0<=entry<len(data): raise ValueError("entry outside body")
    pending=[entry];visited=set();owners={};unresolved=[];interrupts=[]
    while pending:
        start=pending.pop()
        if start in visited: continue
        if not 0<=start<len(data):
            unresolved.append(dict(offset=start,reason="edge outside executable object"));continue
        result=subprocess.run(["ndisasm","-b",str(bits),"-o",str(start),"-"],
                              input=data[start:start+1024],capture_output=True,timeout=5,check=True)
        previous=None
        for line in result.stdout.decode("ascii").splitlines():
            match=re.fullmatch(r"([0-9A-F]+)\s+([0-9A-F]+)\s+(.+)",line)
            if not match: continue
            at=int(match[1],16);raw=bytes.fromhex(match[2]);text=match[3];next_at=at+len(raw)
            if at in visited: break
            if any(byte in owners and owners[byte]!=at for byte in range(at,next_at)):
                unresolved.append(dict(offset=at,reason="conflicting decoded instruction boundaries"));break
            if len(visited)>=limit: raise ValueError("static instruction budget exhausted")
            visited.add(at)
            for byte in range(at,next_at): owners[byte]=at
            op,*operands=text.split(None,1);operand=operands[0] if operands else ""
            while op in ("rep","repe","repne","lock","a16","a32","o16","o32") and operand:
                op,*operands=operand.split(None,1);operand=operands[0] if operands else ""
            operand=operand.removeprefix("byte ")
            if op in ("db","ud0","ud1","ud2"):
                unresolved.append(dict(offset=at,reason="invalid instruction"));break
            if op=="int" and operand in ("0x21","0x2f"):
                selector=None
                if previous:
                    if len(previous)==2 and previous[0]==0xb4: selector=f"{previous[1]:02X}??"
                    if len(previous) in (3,5) and previous[0]==0xb8:
                        selector=f"{int.from_bytes(previous[1:],'little')&0xffff:04X}"
                interrupts.append(dict(offset=at,vector=operand[2:],ax=selector,evidence="static-direct-reachability"))
            if op in ("ret","retf","iret","iretd","hlt") or (op=="int" and operand=="0x20"): break
            branch=op.startswith("j") or op.startswith("loop") or op=="call"
            if branch:
                target=re.fullmatch(r"(?:short |near )?(0x[0-9a-f]+)",operand)
                if target: pending.append(int(target[1],16))
                else: unresolved.append(dict(offset=at,reason="indirect/far transfer",instruction=text))
                if op!="jmp" and next_at<len(data): pending.append(next_at)
                break
            previous=raw
        else:
            if next_at<len(data): pending.append(next_at)
    return dict(instructions=len(visited),interrupts=sorted(interrupts,key=lambda r:r["offset"]),
                unresolved=unresolved,complete=not unresolved)


def extent(data, start, size):
    if start<0 or size<0 or start+size>len(data): raise ValueError("executable extent out of range")
    return data[start:start+size]


def mz_end(data, start=0):
    if extent(data,start,2)!=b"MZ": raise ValueError("missing MZ stub")
    last,pages=struct.unpack_from("<HH",extent(data,start+2,4))
    if not pages or last>=512: raise ValueError("invalid MZ length")
    end=start+pages*512-(512-last if last else 0)
    extent(data,start,end-start)
    return end


def coff(data):
    """DJGPP v2 MZ stub + COFF, offsets relative to the COFF header."""
    base=mz_end(data)
    header=extent(data,base,20)
    magic,count,_,_,_,optional,flags=struct.unpack("<HHIIIHH",header)
    if magic!=0x14c or not 1<=count<=32 or optional<28 or not flags&2:
        raise ValueError("invalid executable DJGPP COFF header")
    opt=extent(data,base+20,optional)
    if struct.unpack_from("<H",opt)[0]!=0x10b: raise ValueError("unknown COFF optional header")
    entry=struct.unpack_from("<I",opt,16)[0]
    sections=[]
    for i in range(count):
        row=extent(data,base+20+optional+i*40,40)
        name=row[:8].rstrip(b"\0").decode("ascii")
        _,address,size,offset=struct.unpack_from("<4I",row,8)
        section_flags=struct.unpack_from("<I",row,36)[0]
        if size>16*1024*1024: raise ValueError("COFF section exceeds budget")
        if not section_flags&0x20: continue
        body=extent(data,base+offset,size)
        sections.append(dict(name=name,address=address,offset=base+offset,size=size,body=body))
    matches=[r for r in sections if r["address"]<=entry<r["address"]+r["size"]]
    if len(matches)!=1: raise ValueError("COFF entry not uniquely in executable section")
    return sections,matches[0],entry-matches[0]["address"]


def linear(data):
    """Find a structurally linked bound MZ/LE module, then map object pages.

    Scanning MZ signatures locates envelopes only. A matching e_lfanew, full
    tables, page extents and executable entry are required before decoding.
    """
    candidates=[]
    for match in re.finditer(b"MZ",data):
        base=match.start()
        if base+64>len(data): continue
        offset=struct.unpack_from("<I",data,base+0x3c)[0];at=base+offset
        if offset>=64 and at+0xc4<=len(data) and data[at:at+4]==b"LE\0\0": candidates.append((base,at))
    if len(candidates)!=1: raise ValueError("missing/ambiguous bound LE module")
    base,at=candidates[0]
    def word(off): return struct.unpack_from("<I",data,at+off)[0]
    if word(4)!=0: raise ValueError("unsupported LE format level")
    pages=word(0x14);entry_object=word(0x18);entry=word(0x1c);page_size=word(0x28);last_size=word(0x2c)
    count=word(0x44);table=at+word(0x40);page_table=at+word(0x48);data_pages=base+word(0x80)
    if not 1<=count<=64 or not 1<=pages<=8192 or page_size!=4096 or not 0<last_size<=page_size:
        raise ValueError("unsupported LE object/page geometry")
    extent(data,page_table,pages*4);objects=[]
    for i in range(count):
        size,address,flags,first,n,_=struct.unpack("<6I",extent(data,table+i*24,24))
        if size>16*1024*1024 or (n and (not first or first+n-1>pages)) or size>n*page_size and flags&4:
            raise ValueError("LE object extent invalid")
        if not flags&4: continue
        body=bytearray()
        for index in range(first-1,first-1+n):
            row=extent(data,page_table+index*4,4);physical=int.from_bytes(row[:3],"big")
            if row[3]==3: body.extend(bytes(page_size));continue
            if row[3]!=0 or not 1<=physical<=pages: raise ValueError("unsupported LE page mapping")
            amount=last_size if physical==pages else page_size
            body.extend(extent(data,data_pages+(physical-1)*page_size,amount));body.extend(bytes(page_size-amount))
        objects.append(dict(index=i+1,address=address,size=size,bits=32 if flags&0x2000 else 16,body=bytes(body[:size])))
    matches=[r for r in objects if r["index"]==entry_object and entry<r["size"]]
    if len(matches)!=1: raise ValueError("LE entry outside executable object")
    return objects,matches[0],entry


def protected_audit(data, reader):
    objects,main,entry=reader(data)
    result=[]
    for row in objects:
        metadata={k:v for k,v in row.items() if k!="body"}
        metadata["load_sha256"]=hashlib.sha256(row["body"]).hexdigest()
        if row is main: metadata["static"]=reachable(row["body"],entry,row.get("bits",32))
        else: metadata["status"]="no-proven-entry; relocation/runtime-evidence-required"
        result.append(metadata)
    return dict(original_entry=entry,objects=result,relocations="not-applied; absolute/indirect edges remain open")


@lru_cache(maxsize=1)
def collect():
    spec=importlib.util.spec_from_file_location("d51_games",ROOT/"scripts/build_games_disk.py")
    games=importlib.util.module_from_spec(spec);spec.loader.exec_module(games)
    result=dict(schema_version=1,runtime_trace="not-collected",files=[])
    # Pinned f3-51 corpus: the three shareware games (see inventory.CORPUS).
    for package in CORPUS:
        _,members,_=games.package_files(package)
        for path,data,_ in members:
            if not path.upper().endswith((".COM",".EXE")): continue
            row=dict(path=path,sha256=hashlib.sha256(data).hexdigest())
            if data[28:32] in (b"LZ91",b"LZ09"):
                body,entry=lzexe(data)
                row.update(format="LZEXE",load_bytes=len(body),load_sha256=hashlib.sha256(body).hexdigest(),
                           original_entry=entry,static=reachable(body,entry),candidate_count=len(sites(body)))
            elif path=="/DOOM/DOOM.EXE":
                row.update(format="DOS/4GW-LE",**protected_audit(data,linear))
            elif path=="/QUAKE/QUAKE.EXE":
                row.update(format="go32-COFF",**protected_audit(data,coff))
            else:
                row.update(format="go32" if b"go32stub" in data else "DOS/4GW" if b"DOS/4GW" in data else "other",
                           status="body-format-and-runtime-reachability-open")
            result["files"].append(row)
    return result

if __name__=="__main__":
    print(json.dumps(collect(),indent=2))

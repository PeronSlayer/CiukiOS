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
from inventory import ROOT, sites


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


def reachable(data, entry, bits=16):
    """Follow decoded direct edges; report every unresolved transfer/boundary.

    A direct target that is not an instruction start in this decode is open,
    rather than globally reclassifying all adjacent bytes as instructions.
    """
    if not 0<=entry<len(data): raise ValueError("entry outside body")
    result=subprocess.run(["ndisasm","-b",str(bits),"-e",str(entry),"-o",str(entry),"-"],
                          input=data,capture_output=True,timeout=30,check=True)
    instructions={}
    for line in result.stdout.decode("ascii").splitlines():
        match=re.fullmatch(r"([0-9A-F]+)\s+([0-9A-F]+)\s+(.+)",line)
        if match:
            at=int(match[1],16); instructions[at]=(bytes.fromhex(match[2]),match[3])
    ending={at+len(raw):(at,raw,text) for at,(raw,text) in instructions.items()}
    pending=[entry];visited=set();unresolved=[];interrupts=[]
    while pending:
        at=pending.pop()
        if at in visited: continue
        if at not in instructions:
            unresolved.append(dict(offset=at,reason="target not in decoded instruction boundaries"));continue
        visited.add(at);raw,text=instructions[at];next_at=at+len(raw)
        op,*operands=text.split(None,1);operand=operands[0] if operands else ""
        while op in ("rep","repe","repne","lock","a16","a32","o16","o32") and operand:
            op,*operands=operand.split(None,1);operand=operands[0] if operands else ""
        operand=operand.removeprefix("byte ")
        if op in ("ret","retf","iret","iretd","hlt") or (op=="int" and operand=="0x20"): continue
        if op in ("db","ud0","ud1","ud2"):
            unresolved.append(dict(offset=at,reason="invalid instruction"));continue
        if op=="int" and operand in ("0x21","0x2f"):
            # Exact immediate load at the preceding decoded boundary, on a
            # traversed edge. It is a possible selector, never an observed call.
            previous=[ending[at]] if at in ending and ending[at][0] in visited else []
            selector=None
            if previous:
                _,before,_=previous[0]
                if len(before)==2 and before[0]==0xb4: selector=f"{before[1]:02X}??"
                if len(before)==3 and before[0]==0xb8: selector=f"{int.from_bytes(before[1:],'little'):04X}"
            interrupts.append(dict(offset=at,vector=operand[2:],ax=selector,evidence="static-direct-reachability"))
        branch=op.startswith("j") or op.startswith("loop") or op=="call"
        if branch:
            target=re.fullmatch(r"(?:short |near )?(0x[0-9a-f]+)",operand)
            if target: pending.append(int(target[1],16))
            else: unresolved.append(dict(offset=at,reason="indirect/far transfer",instruction=text))
            if op=="jmp": continue
        if next_at<len(data): pending.append(next_at)
    return dict(instructions=len(visited),interrupts=sorted(interrupts,key=lambda r:r["offset"]),
                unresolved=unresolved,complete=not unresolved)


def collect():
    spec=importlib.util.spec_from_file_location("d51_games",ROOT/"scripts/build_games_disk.py")
    games=importlib.util.module_from_spec(spec);spec.loader.exec_module(games)
    result=dict(schema_version=1,runtime_trace="not-collected",files=[])
    for package in games.PACKAGES:
        _,members,_=games.package_files(package)
        for path,data,_ in members:
            if not path.upper().endswith((".COM",".EXE")): continue
            row=dict(path=path,sha256=hashlib.sha256(data).hexdigest())
            if data[28:32] in (b"LZ91",b"LZ09"):
                body,entry=lzexe(data)
                row.update(format="LZEXE",load_bytes=len(body),load_sha256=hashlib.sha256(body).hexdigest(),
                           original_entry=entry,static=reachable(body,entry),candidate_count=len(sites(body)))
            else:
                row.update(format="go32" if b"go32stub" in data else "DOS/4GW" if b"DOS/4GW" in data else "other",
                           status="body-format-and-runtime-reachability-open")
            result["files"].append(row)
    return result

if __name__=="__main__":
    print(json.dumps(collect(),indent=2))

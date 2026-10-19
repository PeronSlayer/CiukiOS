"""Canonical DOS conformance payloads and strict built-image read-back.

Never downloads media or builds a variant. FreeDOS release packages must first
be provisioned and pinned under third_party/freedos/1.3 by the lead. No master
binary is accepted as a reference. SPDX-License-Identifier: GPL-2.0-only
"""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RELEASE = ROOT / "third_party/freedos/1.3"
OFFICIAL = "https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/"
CONFIG = b"DOS=LOW\r\nFILES=40\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\nSHELL=C:\\COMMAND.COM C:\\ /P=C:\\FDAUTO.BAT /E:1024\r\n"
AUTO = b"@echo off\r\nC:\\SHARE.COM\r\nSET PATH=C:\\SYSTEM;C:\\D48;C:\\\r\nSET BLASTER=A220 I7 D1 H5 T6\r\nC:\\DOSREADY.COM\r\n"
PROFILE = dict(share=True, ansi=False, redirector=False, dpmi=True, xms=True,
               freedos_bridge=False, ciukidos_bridge=True, hardware="qemu-t23")
REFERENCE_START = 63


def reference_offset(data):
    if len(data)<512 or data[510:512]!=b"\x55\xaa": raise ValueError("reference MBR absent")
    partition=data[446:462]
    if (partition[0]!=0x80 or partition[4]!=0x0e or
            struct.unpack_from("<II",partition,8)!=(REFERENCE_START,262144-REFERENCE_START) or any(data[462:510])):
        raise ValueError("reference partition geometry mismatch")
    return REFERENCE_START*512


def sha(data):
    return hashlib.sha256(data).hexdigest()


def module(name, path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec); sys.modules[name]=result
    spec.loader.exec_module(result)
    return result


def probes(out):
    sys.path.insert(0,str(ROOT/"tests/host/dos"))
    from conformance import build
    from protocol import probe_manifest
    manifest=build(out)
    probe_manifest(manifest,lambda name:(out/name).read_bytes())
    files={"/D48/"+row["binary"]:(out/row["binary"]).read_bytes()
           for row in manifest["cases"]+manifest["helpers"]}
    files["/D48/MANIFEST.JSN"]=(out/"manifest.json").read_bytes()
    # D48DONE executes only after the original COM returns to the shell.
    subprocess.run(["nasm","-f","bin",str(ROOT/"tests/host/dos/shell_ready.asm"),
                    "-o",str(out/"D48DONE.COM")],check=True)
    files["/D48/D48DONE.COM"]=(out/"D48DONE.COM").read_bytes()
    files["/D48/CONTROL.JSN"]=(json.dumps({"shell_ready_sha256":sha(files["/D48/D48DONE.COM"])},sort_keys=True)+"\n").encode()
    rows=[]
    for row in manifest["cases"]:
        rows.append(row["tag"]+" "+row["fixture"])
    files["/D48/PLAN.TXT"]=("\n".join(rows)+"\n").encode("ascii")
    (out/"PLAN.TXT").write_bytes(files["/D48/PLAN.TXT"])
    return files,manifest


def reference_files(folder=RELEASE):
    """Read exact release package/member pins, including redistribution notices.

    No TOFU pin generation: missing pins fail. The report records this as open
    until the lead obtains official archives. Archive paths cannot escape folder.
    """
    folder=Path(folder)
    path=folder/"UPSTREAM.json"
    if not path.is_file():
        raise ValueError("FreeDOS 1.3 reference absent: provision official packages and UPSTREAM.json in third_party/freedos/1.3")
    pin=json.loads(path.read_text())
    if pin.get("release")!="1.3" or pin.get("distribution_url")!=OFFICIAL:
        raise ValueError("reference must be the official FreeDOS 1.3 distribution")
    files={}; identities={}
    if set(pin)-{"release","distribution_url","kernel","shell","share","boot","boot_fat16"}:
        raise ValueError("unknown reference pin key")
    for role,target in (("kernel","/KERNEL.SYS"),("shell","/COMMAND.COM"),("share","/SHARE.COM")):
        item=pin.get(role,{})
        if not item.get("version") or not item.get("license") or not item.get("license_member"):
            raise ValueError("reference version/license missing: "+role)
        url=item.get("url","")
        if not url.startswith((OFFICIAL,"https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/")):
            raise ValueError("unapproved reference package URL")
        name=item.get("archive","")
        if not re.fullmatch(r"[a-zA-Z0-9_.-]+\.zip",name):
            raise ValueError("invalid reference archive path")
        data=(folder/name).read_bytes()
        if sha(data)!=item.get("package_sha256"):
            raise ValueError("reference package hash mismatch: "+role)
        with zipfile.ZipFile(io.BytesIO(data)) as package:
            if len(package.namelist())!=len(set(package.namelist())):
                raise ValueError("duplicate reference archive member")
            for member in (item.get("member",""),item["license_member"]):
                info=package.getinfo(member)
                if info.file_size>4*1024*1024:
                    raise ValueError("reference member exceeds bound")
            binary=package.read(item["member"])
            license_data=package.read(item["license_member"])
        if sha(binary)!=item.get("sha256") or sha(license_data)!=item.get("license_sha256"):
            raise ValueError("reference binary/license hash mismatch: "+role)
        files[target]=binary; files["/D48/"+role.upper()+".LIC"]=license_data
        identities[role]=item
    return files,identities


def reference_boot(folder, item):
    """An official 1.3 archive member, optionally a boot sector within media.
    Never compile a master boot loader to stand in for release provenance.
    """
    name=item.get("archive","")
    if not re.fullmatch(r"[a-zA-Z0-9_.-]+\.zip",name) or not item.get("license") or not item.get("version"):
        raise ValueError("official FreeDOS 1.3 boot template pin required")
    if not item.get("url","").startswith((OFFICIAL,"https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/")):
        raise ValueError("boot template must come from official release media")
    data=(Path(folder)/name).read_bytes()
    if sha(data)!=item.get("package_sha256"): raise ValueError("boot package hash mismatch")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        member=item.get("member","")
        info=archive.getinfo(member)
        if info.file_size>4*1024*1024 or archive.namelist().count(member)!=1:
            raise ValueError("invalid boot media member")
        raw=archive.read(member)
    if sha(raw)!=item.get("member_sha256"): raise ValueError("boot member hash mismatch")
    if item.get("template"):
        if item["template"]!="boot16.bin" or item.get("member")!="BIN/SYS.COM": raise ValueError("unknown release boot template")
        boot=(Path(folder)/item["template"]).read_bytes()
        if len(boot)!=512 or boot[54:62]!=b"FAT16   " or boot[510:]!=b"\x55\xaa" or sha(boot)!=item.get("sha256"):
            raise ValueError("release FAT16 template hash/signature mismatch")
        return boot
    offset=item.get("offset",0)
    if type(offset)!=int or offset<0 or offset+512>len(raw): raise ValueError("boot sector offset out of bounds")
    boot=raw[offset:offset+512]
    if boot[510:]!=b"\x55\xaa" or sha(boot)!=item.get("sha256"):
        raise ValueError("boot template hash/signature mismatch")
    return boot


def installed_boot(folder=RELEASE):
    pin=json.loads((Path(folder)/"UPSTREAM.json").read_text())["boot_fat16"]
    boot=bytearray(reference_boot(folder,pin))
    dos=module("d51_dos_geometry",ROOT/"scripts/build_dos_image.py")
    struct.pack_into("<HBHBHHBHHHII",boot,11,512,dos.SPC,dos.RESERVED,2,512,0,0xf8,
                     dos.FAT_SECTORS,63,16,REFERENCE_START,dos.SECTORS-REFERENCE_START)
    boot[3:11]=b"FRDOS5.1";boot[36]=0x80;boot[38]=0x29;boot[43:54]=b"FREEDOS13  "
    return bytes(boot),pin


def reference_mbr():
    result=subprocess.run(["nasm","-f","bin",str(ROOT/"tests/host/dos/reference_mbr.asm"),"-o","/dev/stdout"],
                          check=True,capture_output=True)
    reference_offset(result.stdout)
    return result.stdout


def read_file(image, path, offset=0):
    """Bounded, independent FAT16/32 read-back, supports nested private images.

    Small payloads only; full canonical images are never read/copied into RAM.
    Reject loops, ambiguous names, truncated chains and out-of-range extents.
    """
    own=not hasattr(image,"read")
    stream=Path(image).open("rb") if own else image
    try:
        stream.seek(0,2); total=stream.tell()
        def read(at,n):
            if at<0 or n<0 or offset+at+n>total: raise ValueError("FAT extent out of range")
            stream.seek(offset+at); data=stream.read(n)
            if len(data)!=n: raise ValueError("short FAT read")
            return data
        boot=read(0,512)
        if boot[510:]!=b"\x55\xaa" or struct.unpack_from("<H",boot,11)[0]!=512:
            raise ValueError("invalid FAT boot record")
        spc=boot[13]; reserved=struct.unpack_from("<H",boot,14)[0]; fats=boot[16]
        roots=struct.unpack_from("<H",boot,17)[0]
        sectors=struct.unpack_from("<H",boot,19)[0] or struct.unpack_from("<I",boot,32)[0]
        fat_sectors=struct.unpack_from("<H",boot,22)[0] or struct.unpack_from("<I",boot,36)[0]
        if not spc or spc&(spc-1) or not fats or not reserved or not fat_sectors: raise ValueError("bad FAT geometry")
        root_start=(reserved+fats*fat_sectors)*512; root_bytes=((roots*32+511)//512)*512
        data_start=root_start+root_bytes; clusters=(sectors-data_start//512)//spc
        kind=16 if 4085<=clusters<65525 else 32 if clusters>=65525 and not roots else 0
        if not kind or clusters+2>fat_sectors*512//(kind//8): raise ValueError("bad FAT capacity")
        def chain(first):
            result=bytearray(); seen=set(); cluster=first
            while True:
                if cluster<2 or cluster>=clusters+2 or cluster in seen: raise ValueError("invalid/cyclic FAT chain")
                seen.add(cluster)
                if len(result)+spc*512>160*1024*1024: raise ValueError("FAT payload exceeds bound")
                result.extend(read(data_start+(cluster-2)*spc*512,spc*512))
                raw=read(reserved*512+cluster*(kind//8),kind//8)
                cluster=int.from_bytes(raw,"little")&(0xffff if kind==16 else 0xfffffff)
                if cluster >= (0xfff8 if kind==16 else 0xffffff8): return bytes(result)
        directory=read(root_start,root_bytes) if kind==16 else chain(struct.unpack_from("<I",boot,44)[0])
        parts=path.strip("/").upper().split("/")
        for index,name in enumerate(parts):
            found=[]
            for at in range(0,len(directory),32):
                row=directory[at:at+32]
                if not row[0]: break
                if row[0]==0xe5 or row[11]&8: continue
                leaf=row[:8].decode("ascii").rstrip()+(("."+row[8:11].decode("ascii").rstrip()) if row[8:11].strip() else "")
                if leaf==name: found.append(row)
            if len(found)!=1: raise ValueError("missing/ambiguous FAT payload: "+path)
            row=found[0]; first=struct.unpack_from("<H",row,26)[0]
            if kind==32: first|=struct.unpack_from("<H",row,20)[0]<<16
            size=struct.unpack_from("<I",row,28)[0]
            if index+1<len(parts):
                if not row[11]&16: raise ValueError("FAT parent is not directory")
                directory=chain(first)
            else:
                if row[11]&16: raise ValueError("FAT payload is directory")
                result=chain(first) if size else b""
                if len(result)<size: raise ValueError("truncated FAT payload")
                return result[:size]
    finally:
        if own: stream.close()


def readback(image, probes_manifest):
    """Generate integration identities from the canonical disk, never a flag."""
    import conformance
    from protocol import probe_manifest
    offset=2048*512
    candidate=read_file(image,"/SYSTEM/DOS/DOSBOOT.IMG",offset)
    reference=read_file(image,"/SYSTEM/DOS/FREEDOS.IMG",offset)
    reference_hash=read_file(image,"/SYSTEM/DOS/FREEDOS.SHA",offset)
    if reference_hash!=(sha(reference)+"\n").encode(): raise ValueError("reference sidecar hash mismatch")
    ref_offset=reference_offset(reference)
    def member(data,path):
        return read_file(io.BytesIO(data),path,ref_offset if data is reference else 0)
    manifest_data=read_file(io.BytesIO(candidate),"/D48/MANIFEST.JSN")
    if manifest_data!=Path(probes_manifest).read_bytes(): raise ValueError("embedded probe manifest mismatch")
    expected=json.loads(manifest_data)
    control=read_file(io.BytesIO(candidate),"/D48/CONTROL.JSN")
    for data in (candidate,reference):
        if (member(data,"/D48/CONTROL.JSN")!=control or
                sha(member(data,"/D48/D48DONE.COM"))!=json.loads(control)["shell_ready_sha256"]):
            raise ValueError("shell-return helper read-back mismatch")
        embedded=member(data,"/D48/MANIFEST.JSN")
        if embedded!=manifest_data: raise ValueError("reference/candidate probe manifest mismatch")
        probe_manifest(json.loads(embedded),lambda name:member(data,"/D48/"+name))
        if member(data,"/D48/PLAN.TXT")!=read_file(io.BytesIO(candidate),"/D48/PLAN.TXT"):
            raise ValueError("reference/candidate plan mismatch")
    if read_file(image,"/SYSTEM/DOS/D48PLAN.TXT",offset)!=read_file(io.BytesIO(candidate),"/D48/PLAN.TXT"):
        raise ValueError("external runtime plan differs from private fixture")
    expected_plan=("\n".join(r["tag"]+" "+r["fixture"] for r in expected["cases"])+"\n").encode()
    if expected_plan!=read_file(io.BytesIO(candidate),"/D48/PLAN.TXT"):
        raise ValueError("runtime plan differs from authoritative cases")
    identity=json.loads(member(reference,"/D48/REF.JSN"))
    _,pinned=reference_files()
    result=dict(schema_version=1,dos_conformance_adapter=1,image_sha256=conformance.digest(image),
                probes_sha256=sha(manifest_data),freedos_release="1.3",read_back="canonical-image",profile=PROFILE,payloads=[],
                controller_sha256=sha(control),plan_sha256=sha(read_file(io.BytesIO(candidate),"/D48/PLAN.TXT")))
    for role,path in (("kernel","/KERNEL.SYS"),("shell","/COMMAND.COM"),("share","/SHARE.COM")):
        data=member(reference,path); row=identity[role]
        if row!=pinned[role] or sha(data)!=row["sha256"]: raise ValueError("reference identity/read-back mismatch")
        if sha(member(reference,"/D48/"+role.upper()+".LIC"))!=row["license_sha256"]:
            raise ValueError("reference license read-back mismatch")
        result["freedos_"+role+"_sha256"]=sha(data); result["freedos_"+role]=row
        result["payloads"].append(dict(path=path,sha256=sha(data),size=len(data)))
    config=member(reference,"/FDCONFIG.SYS")
    auto=member(reference,"/FDAUTO.BAT")
    if config!=CONFIG or auto!=AUTO:
        raise ValueError("reference configuration does not match SHARE/no-ANSI/no-redirector profile")
    profile=config+auto
    result["freedos_profile_sha256"]=sha(profile)
    boot=reference[ref_offset:ref_offset+512]
    expected_boot,boot_pin=installed_boot()
    if (boot!=expected_boot or identity.get("boot")!=dict(boot_pin,installed_sha256=sha(expected_boot))):
        raise ValueError("reference boot read-back mismatch")
    result["freedos_boot_sha256"]=sha(boot)
    result["freedos_boot"]=identity["boot"]
    if reference[:512]!=reference_mbr() or sha(reference[:512])!=identity.get("mbr_sha256"):
        raise ValueError("reference MBR identity mismatch")
    result["freedos_image_sha256"]=sha(reference)
    result["freedos_mbr_sha256"]=sha(reference[:512])
    result["freedos_partition_lba"]=REFERENCE_START
    return result


def build_reference(out, system_files, directories, games_files=None, folder=RELEASE):
    """Canonical build helper, called once by build_image; never by test runs."""
    reference,identity=reference_files(folder)
    dos=module("d51_dos_build",ROOT/"scripts/build_dos_image.py")
    out=Path(out); out.parent.mkdir(parents=True,exist_ok=True)
    boot,boot_pin=installed_boot(folder)
    files=dict(system_files)
    files.pop("/SYSTEM/CIUKIDOS.SYS",None)
    files.pop("/AUTOEXEC.BAT",None)
    if games_files: files.update(games_files)
    files.update(reference)
    files["/FDCONFIG.SYS"]=CONFIG
    files["/FDAUTO.BAT"]=AUTO
    mbr=reference_mbr()
    identity["mbr_sha256"]=sha(mbr)
    identity["boot"]=dict(boot_pin,installed_sha256=sha(bytes(boot)))
    files["/D48/REF.JSN"]=(json.dumps(identity,sort_keys=True,indent=2)+"\n").encode()
    dos.write_fat16(out,bytes(boot),b"",files,directories=directories,label="FREEDOS13",sectors=dos.SECTORS-REFERENCE_START)
    subprocess.run(["fsck.fat","-n",str(out)],check=True,stdout=subprocess.DEVNULL)
    for path,data in files.items():
        if read_file(out,path)!=data: raise ValueError("FreeDOS read-back mismatch: "+path)
    # Trim only trailing unused zero sectors, as for the existing games base.
    used=512
    with out.open("rb") as stream:
        while block:=stream.read(65536):
            if block.rstrip(b"\0"): used=stream.tell()-len(block)+len(block.rstrip(b"\0"))
    with out.open("r+b") as stream: stream.truncate((used+511)//512*512)
    # Shift the small used private payload in place. Never copy the canonical
    # full image or change its VMM disk capacity. The ordinary FAT BPB carries
    # the hidden-sector count used by the exact pinned FreeDOS VBR.
    with out.open("r+b") as stream:
        stream.seek(0,2); remaining=stream.tell()
        while remaining:
            n=min(65536,remaining);remaining-=n
            stream.seek(remaining);block=stream.read(n)
            stream.seek(REFERENCE_START*512+remaining);stream.write(block)
        stream.seek(0);stream.write(mbr+bytes((REFERENCE_START-1)*512))
    for path,data in files.items():
        if read_file(out,path,REFERENCE_START*512)!=data: raise ValueError("partitioned reference read-back mismatch: "+path)
    hash_data=(dos.digest(out)+"\n").encode("ascii")
    out.with_suffix(".SHA").write_bytes(hash_data)
    return identity

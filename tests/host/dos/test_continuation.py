"""Amendment 1 host evidence. Never execute QEMU or build a disk image."""
import copy
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile
import conformance
import overlay
import trace as audit
import reachability
import test_integration
from reference_fixture import provisioned_reference

install=test_integration.install


class FixtureTests(unittest.TestCase):
    def test_production_after_open_fault_boundary(self):
        parent=conformance.ROOT/"build/host/dos";parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as d:
            binary=Path(d)/"fixture"
            subprocess.run(["clang","-DFS_HOST","-D_POSIX_C_SOURCE=200809L","-std=c17","-O1","-g","-Wall","-Wextra","-Werror",
                "-fsanitize=address,undefined","-I",str(conformance.ROOT/"src/kernel/include"),
                str(Path(__file__).with_name("fixture_test.c")),"-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"))

    def test_actual_provisioned_release_and_retry_source(self):
        folder=provisioned_reference(self)
        files,identity=install.reference_files(folder)
        self.assertEqual(identity["kernel"]["version"],"2043")
        for role,path in (("kernel","/KERNEL.SYS"),("shell","/COMMAND.COM"),("share","/SHARE.COM")):
            self.assertEqual(install.sha(files[path]),identity[role]["sha256"])
        pin=json.loads((folder/"UPSTREAM.json").read_text())
        boot=install.reference_boot(folder,pin["boot"])
        self.assertEqual(boot[3:11],b"FRDOS5.1")
        fat16=install.reference_boot(folder,pin["boot_fat16"])
        self.assertEqual(fat16[54:62],b"FAT16   ")
        with zipfile.ZipFile(folder/"kernel.zip") as archive:
            raw=archive.read("SOURCE/KERNEL/SOURCES.ZIP")
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            self.assertRegex(archive.read("hdr/device.h").decode(),r"#define N_RETRY\s+5\b")
            self.assertIn(b"num_retries < N_RETRY",archive.read("kernel/dsk.c"))

    def test_reproduce_release_fat16_template_without_running_sys(self):
        folder=provisioned_reference(self)
        parent=conformance.ROOT/"build/host/dos";parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as d:
            output=Path(d);binary=output/"unpack";source=output/"SYS.COM"
            with zipfile.ZipFile(folder/"kernel.zip") as archive:source.write_bytes(archive.read("BIN/SYS.COM"))
            subprocess.run(["clang","-std=c17","-O1","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
                str(Path(__file__).with_name("reference_boot_test.c")),"-lx86emu","-o",str(binary)],check=True)
            result=subprocess.run([str(binary),str(source)],capture_output=True,check=True,timeout=10,
                                  env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"))
            self.assertEqual(result.stdout,(folder/"boot16.bin").read_bytes())


class TraceTests(unittest.TestCase):
    def wire(self,seq,extra):
        return f"CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=dos-conformance event=DATA {extra}"

    def streams(self):
        lines=[self.wire(1,"group=start"),self.wire(2,"group=disk_only"),
               self.wire(3,"group=record_ledger first=1 last=2 records=2")]
        return "\n".join([lines[0],lines[2]]),"\n".join(lines)

    def test_disk_only_gap_requires_reconciliation(self):
        serial,disk=self.streams()
        self.assertEqual(len(audit.reconcile(serial,disk,"12345678")),2)
        with self.assertRaisesRegex(ValueError,"gaps"):audit.reconcile(serial,"","12345678")
        for s,d in ((serial+"\n"+serial,disk),(serial,disk.replace("group=start","group=changed")),
                    (serial.replace("12345678","deadbeef"),disk),(serial,disk+"\n"+disk)):
            with self.subTest(s=s),self.assertRaises(ValueError):audit.reconcile(s,d,"12345678")
        for replacement in ("last=1","last=3"):
            with self.assertRaises(ValueError):audit.reconcile(serial.replace("last=2",replacement),disk.replace("last=2",replacement),"12345678")
        serial+="\n"+self.wire(4,"group=second_vm_end")+"\n"+self.wire(5,"group=record_ledger first=1 last=4 records=4")
        self.assertEqual(len(audit.reconcile(serial,disk,"12345678")),4)
        # A selected run can follow unrelated boot diagnostics; its watermark
        # starts at the actual first sequence without resetting global seq.
        serial="\n".join((self.wire(12,"group=start"),self.wire(14,"group=record_ledger first=12 last=13 records=2")))
        disk=self.wire(13,"group=disk_only")
        self.assertEqual(len(audit.reconcile(serial,disk,"12345678")),2)
        with self.assertRaises(ValueError):audit.reconcile(serial+"\n"+self.wire(15,"group=record_ledger first=13 last=14 records=2"),disk,"12345678")

    def records(self):
        rows=[]
        for index,edge in ((1,0),(2,1)):
            for part,keys in audit.PART_KEYS.items():
                row={key:"0000" if key in audit.HEX else "0" for key in keys}
                row.update(group="dos_api",vm="1",gen="7",row=str(index),part=str(part),seq=str(len(rows)+1))
                if part==1:row.update(call="1",edge=str(edge),vector="21",psp="2000")
                if part==2:row["ax"]="3F00" if edge==0 else "0001"
                rows.append(row)
        rows.append(dict(group="dos_trace_end",vm="1",gen="7",records="2",pending="0"))
        return rows

    def test_all_fragments_pairs_coverage_and_declared_failures(self):
        good=self.records();calls=audit.calls(good)
        self.assertEqual(audit.inventory(calls)[0]["selector"],"21:3F00")
        self.assertIn("registers",audit.inventory(calls)[0])
        for bad in (good[:-1],good[1:],good[:3]+good[4:],good+[good[0]],good[:2]+[good[1]]+good[2:]):
            with self.subTest(bad=bad[-1]),self.assertRaises(ValueError):audit.calls(bad)
        wrong=copy.deepcopy(good);wrong[-1]["pending"]="1"
        with self.assertRaises(ValueError):audit.calls(wrong)
        calls[0]["result"]["flags"]=1
        with self.assertRaisesRegex(ValueError,"unsupported"):audit.inventory(calls,approved_errors=[("21:3F00",1)])
        calls[0]["result"]["ax"]=5
        with self.assertRaisesRegex(ValueError,"unapproved failed"):audit.inventory(calls)
        self.assertEqual(len(audit.inventory(calls,approved_errors=[("21:3F00",5)])),1)
        calls[0]["entry"]["ax"]=0xff00
        with self.assertRaisesRegex(ValueError,"unmapped"):audit.inventory(calls)

    def test_title_paths_cannot_be_asserted_without_calls(self):
        with self.assertRaisesRegex(ValueError,"incomplete"):audit.title_paths([],dict(title="doom",paths=[]),[])
        stages=["loader","start","menu","demo","play","save","load","config-write","config-reopen","exit"]
        evidence=dict(title="doom",vm=1,generation=7,paths=[dict(stage=stage,first=i+1,last=i+1) for i,stage in enumerate(stages)])
        records=[dict(seq=str(i+1)) for i in range(len(stages))]
        calls=[dict(entry=dict(wire_sequence=i+1,vm=1,generation=7)) for i in range(len(stages))]
        self.assertEqual(len(audit.title_paths(records,evidence,calls)),10)
        with self.assertRaisesRegex(ValueError,"untraced"):audit.title_paths(records,evidence,calls[:-1])
        calls[-1]["entry"]["vm"]=2
        with self.assertRaisesRegex(ValueError,"untraced"):audit.title_paths(records,evidence,calls)


class OverlayTests(unittest.TestCase):
    def test_actual_fat32_log_reader_without_a_disk_image(self):
        # Sparse virtual stream: only BPB/FAT/directory/data sectors exist in
        # memory. No image builder, large allocation or backing file.
        total=66583;data_start=1058
        blocks={}
        boot=bytearray(512);boot[510:]=b"\x55\xaa"
        struct.pack_into("<H",boot,11,512);boot[13]=1;struct.pack_into("<H",boot,14,32);boot[16]=2
        struct.pack_into("<I",boot,32,total);struct.pack_into("<I",boot,36,513);struct.pack_into("<I",boot,44,2)
        blocks[0]=boot
        fat=bytearray(512)
        for cluster in range(2,6):struct.pack_into("<I",fat,cluster*4,0x0fffffff)
        blocks[32]=fat
        for cluster,name,target,directory in ((2,b"SYSTEM     ",3,True),(3,b"LOGS       ",4,True),(4,b"BOOT    LOG",5,False)):
            row=bytearray(512);row[:11]=name;row[11]=16 if directory else 32
            struct.pack_into("<H",row,26,target);struct.pack_into("<I",row,28,0 if directory else 5)
            blocks[data_start+cluster-2]=row
        blocks[data_start+3]=b"Ciuki"+bytes(507)
        class Sparse:
            at=0
            def seek(self,at,whence=0):self.at=total*512+at if whence==2 else self.at+at if whence==1 else at
            def tell(self):return self.at
            def read(self,n):
                result=bytearray()
                while n:
                    sector,offset=divmod(self.at,512);amount=min(n,512-offset)
                    result.extend(blocks.get(sector,bytes(512))[offset:offset+amount]);self.at+=amount;n-=amount
                return bytes(result)
        self.assertEqual(install.read_file(Sparse(),"/SYSTEM/LOGS/BOOT.LOG"),b"Ciuki")
        struct.pack_into("<I",fat,5*4,5)
        with self.assertRaisesRegex(ValueError,"cyclic"):install.read_file(Sparse(),"/SYSTEM/LOGS/BOOT.LOG")

    def test_readonly_allocated_zero_and_backing_clusters(self):
        with tempfile.TemporaryDirectory() as d:
            base=Path(d)/"base";delta=Path(d)/"overlay"
            base.write_bytes(b"B"*1536);raw=bytearray(2048)
            raw[:4]=b"QFI\xfb";struct.pack_into(">I",raw,4,3);struct.pack_into(">I",raw,20,9)
            struct.pack_into(">Q",raw,24,1536);struct.pack_into(">IIQ",raw,32,0,1,512)
            struct.pack_into(">Q",raw,512,(1<<63)|1024)
            struct.pack_into(">3Q",raw,1024,(1<<63)|1536,1,0);raw[1536:]=b"A"*512
            delta.write_bytes(raw)
            with overlay.Overlay(delta,base) as view:
                self.assertEqual(view.read(),b"A"*512+bytes(512)+b"B"*512)
                view.seek(510);self.assertEqual(view.read(4),b"AA\0\0")
                self.assertEqual(view.seek(0,2),1536)
            for at,value in ((72,1),(1024,1<<62),(512,1<<62)):
                broken=bytearray(raw);struct.pack_into(">Q",broken,at,value);delta.write_bytes(broken)
                with self.assertRaises(ValueError):
                    with overlay.Overlay(delta,base) as view:view.read()
            self.assertEqual(base.read_bytes(),b"B"*1536)


class BodyTests(unittest.TestCase):
    def test_actual_doom_le_quake_coff_entries(self):
        result=reachability.collect()
        for path,kind in (("/DOOM/DOOM.EXE","DOS/4GW-LE"),("/QUAKE/QUAKE.EXE","go32-COFF")):
            row=next(r for r in result["files"] if r["path"]==path)
            self.assertEqual(row["format"],kind)
            self.assertTrue(any(obj.get("static",{}).get("instructions",0)>0 for obj in row["objects"]))
            self.assertTrue(all(len(obj["load_sha256"])==64 for obj in row["objects"]))
        self.assertEqual(result,json.loads((conformance.ROOT/"docs/validation/f3-51-static-reachability.json").read_text()))

    def test_body_bounds_reject_truncated_headers_and_unlinked_patterns(self):
        for reader in (reachability.linear,reachability.coff):
            for data in (b"",b"MZ"+bytes(100),b"MZ"+bytes(100)+b"LE\0\0"):
                with self.subTest(reader=reader,data=data),self.assertRaises(ValueError):reader(data)


if __name__=="__main__":unittest.main()

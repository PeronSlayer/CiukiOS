"""H51 read-back and launcher checks. Synthetic inputs are not guest evidence."""
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile
from unittest.mock import patch
import conformance
import protocol

spec=importlib.util.spec_from_file_location("d51_install",conformance.ROOT/"scripts/dos_conformance.py")
install=importlib.util.module_from_spec(spec);spec.loader.exec_module(install)


class InstallTests(unittest.TestCase):
    def test_canonical_probe_payload_hashes_and_plan(self):
        parent=conformance.ROOT/"build/host/dos";parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as d:
            out=Path(d)
            files,manifest=install.probes(out)
            protocol.probe_manifest(manifest,lambda name:files["/D48/"+name])
            self.assertEqual(files["/D48/PLAN.TXT"],(out/"PLAN.TXT").read_bytes())
            self.assertEqual(len(files["/D48/PLAN.TXT"].splitlines()),len(conformance.tags()))
            self.assertIn(b"D48 SHELL",files["/D48/D48DONE.COM"])
            for tag in conformance.tags(): self.assertIn("/D48/"+tag+".COM",files)

    def pin_fixture(self,folder):
        pin=dict(release="1.3",distribution_url=install.OFFICIAL)
        for role in ("kernel","shell","share"):
            payload=(role+" host fixture, never FreeDOS evidence").encode()
            license_data=b"synthetic license test"
            name=role+".zip"
            with zipfile.ZipFile(folder/name,"w") as z:
                z.writestr("BIN/PAYLOAD",payload);z.writestr("COPYING",license_data)
            pin[role]=dict(archive=name,url=install.OFFICIAL+name,package_sha256=conformance.digest(folder/name),
                           member="BIN/PAYLOAD",sha256=install.sha(payload),version="host-test",license="synthetic",
                           license_member="COPYING",license_sha256=install.sha(license_data))
        (folder/"UPSTREAM.json").write_text(json.dumps(pin))
        return pin

    def test_absent_master_corrupt_packages_and_members(self):
        with tempfile.TemporaryDirectory() as d:
            folder=Path(d)
            with self.assertRaisesRegex(ValueError,"absent"): install.reference_files(folder)
            pin=self.pin_fixture(folder)
            files,identity=install.reference_files(folder)
            self.assertEqual(set(identity),{"kernel","shell","share"})
            self.assertIn("/KERNEL.SYS",files)
            changes=[("release","master"),("distribution_url","https://example.invalid/")]
            for key,value in changes:
                wrong=copy.deepcopy(pin);wrong[key]=value
                (folder/"UPSTREAM.json").write_text(json.dumps(wrong))
                with self.assertRaises(ValueError): install.reference_files(folder)
            for key,value in (("sha256","a"*64),("package_sha256","b"*64),("archive","../kernel.zip"),
                              ("version",""),("license_sha256","c"*64),("url","https://example.invalid/kernel.zip")):
                wrong=copy.deepcopy(pin);wrong["kernel"][key]=value
                (folder/"UPSTREAM.json").write_text(json.dumps(wrong))
                with self.subTest(key=key),self.assertRaises(ValueError): install.reference_files(folder)

    def fat_fixture(self):
        # In-memory FAT16 parser input, 4096 clusters; no build tool/image.
        sectors=4147
        data=bytearray(sectors*512)
        struct.pack_into("<HBHBHHBHHHII",data,11,512,1,1,2,16,sectors,0xf8,17,63,16,0,0)
        data[510:512]=b"\x55\xaa"
        struct.pack_into("<4H",data,512,0xfff8,0xffff,0xffff,0xffff)
        root=(1+34)*512
        data[root:root+11]=b"HELLO   TXT";data[root+11]=32
        struct.pack_into("<H",data,root+26,2);struct.pack_into("<I",data,root+28,5)
        data[(1+34+1)*512:(1+34+1)*512+5]=b"Ciuki"
        return data

    def test_release_boot_template_pin(self):
        with tempfile.TemporaryDirectory() as d:
            folder=Path(d)
            boot=bytes(510)+b"\x55\xaa"; raw=b"prefix"+boot
            with zipfile.ZipFile(folder/"release.zip","w") as archive:
                archive.writestr("BOOT.IMG",raw)
            pin=dict(archive="release.zip",url=install.OFFICIAL+"release.zip",package_sha256=conformance.digest(folder/"release.zip"),
                     member="BOOT.IMG",member_sha256=install.sha(raw),offset=6,sha256=install.sha(boot),license="test-only",version="1.3-test")
            self.assertEqual(install.reference_boot(folder,pin),boot)
            for key,value in (("offset",7),("sha256","a"*64),("member_sha256","b"*64),("version",""),("url","https://example.invalid/")):
                with self.subTest(key=key),self.assertRaises(ValueError):install.reference_boot(folder,dict(pin,**{key:value}))

    def test_real_fat_parser_bounds_and_loops(self):
        data=self.fat_fixture()
        self.assertEqual(install.read_file(io.BytesIO(data),"/HELLO.TXT"),b"Ciuki")
        self.assertEqual(install.read_file(io.BytesIO(b"prefix"+data),"/HELLO.TXT",6),b"Ciuki")
        for name in ("/MISSING.TXT","/HELLO.TXT/NO"):
            with self.assertRaises(ValueError): install.read_file(io.BytesIO(data),name)
        cyclic=bytearray(data);struct.pack_into("<H",cyclic,516,2)
        with self.assertRaises(ValueError): install.read_file(io.BytesIO(cyclic),"HELLO.TXT")
        with self.assertRaises(ValueError): install.read_file(io.BytesIO(data[:1024]),"HELLO.TXT")
        data[510]=0
        with self.assertRaises(ValueError): install.read_file(io.BytesIO(data),"HELLO.TXT")

    def test_settings_production_c(self):
        parent=conformance.ROOT/"build/host/dos";parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as d:
            folder=Path(d);binary=folder/"settings"
            subprocess.run(["clang","-std=c17","-O1","-g","-Wall","-Wextra","-Werror",
                            "-fsanitize=address,undefined","-I",str(conformance.ROOT/"src/kernel/include"),
                            '-DDOSBOX_SETTINGS_DIR="'+str(folder/'prefs')+'"',str(Path(__file__).with_name("settings_test.c")),
                            "-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"))


class ReadbackTests(unittest.TestCase):
    def test_readback_compares_actual_embedded_members_and_profiles(self):
        parent=conformance.ROOT/"build/host/dos";parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as d:
            folder=Path(d)
            files,manifest=install.probes(folder/"probes")
            candidate=dict(files);reference=dict(files)
            binaries,identity=install.reference_files();reference.update(binaries)
            boot,boot_pin=install.installed_boot();mbr=install.reference_mbr()
            reference_blob=mbr+bytes(62*512)+boot
            identity["boot"]=dict(boot_pin,installed_sha256=install.sha(boot))
            identity["mbr_sha256"]=install.sha(mbr)
            reference["/D48/REF.JSON"]=json.dumps(identity).encode()
            reference["/FDCONFIG.SYS"]=install.CONFIG;reference["/FDAUTO.BAT"]=install.AUTO
            image=folder/"not-a-real-image";image.write_bytes(b"mock outer bytes")
            def read(source,path,offset=0):
                if source==image:
                    self.assertEqual(offset,2048*512)
                    return {"/SYSTEM/DOS/DOSBOOT.IMG":b"candidate", "/SYSTEM/DOS/FREEDOS.IMG":reference_blob,
                            "/SYSTEM/DOS/FREEDOS.SHA":(install.sha(reference_blob)+"\n").encode(),
                            "/SYSTEM/DOS/D48PLAN.TXT":files["/D48/PLAN.TXT"]}[path]
                candidate_source=source.getvalue()==b"candidate"
                self.assertEqual(offset,0 if candidate_source else install.REFERENCE_START*512)
                return (candidate if candidate_source else reference)[path]
            with patch.object(install,"read_file",side_effect=read):
                result=install.readback(image,folder/"probes/manifest.json")
                self.assertEqual(result["freedos_kernel_sha256"],identity["kernel"]["sha256"])
                self.assertEqual(result["freedos_profile_sha256"],install.sha(install.CONFIG+install.AUTO))
                for path in ("/D48/D48001.COM","/D48/D48CHILD.COM","/D48/D48DONE.COM","/D48/PLAN.TXT",
                             "/KERNEL.SYS","/D48/KERNEL.LIC","/D48/MANIFEST.JSN","/FDCONFIG.SYS","/FDAUTO.BAT"):
                    saved=reference[path];reference[path]=b"corrupt"
                    with self.subTest(path=path),self.assertRaises((ValueError,KeyError,json.JSONDecodeError)):
                        install.readback(image,folder/"probes/manifest.json")
                    reference[path]=saved

if __name__=="__main__": unittest.main()

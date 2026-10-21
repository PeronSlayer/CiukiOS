"""H51-01: adversarial transcripts; all guest/QEMU activity is mocked."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
import protocol
import conformance


class ProtocolTests(unittest.TestCase):
    def lines(self):
        return ["D48 BEGIN run=12345678 guest=freedos", *[f"{t} PASS 0000" for t in conformance.tags()],
                "D48 CLEAN run=12345678 guest=freedos handles=0 searches=0 locks=0 vms=0 dropped=0",
                f"D48 END run=12345678 guest=freedos count={len(conformance.tags())}"]

    def parse(self, lines):
        parser = protocol.Transcript("12345678", "freedos")
        for line in lines:
            parser.feed(line)
        return parser.finish()

    def test_selector(self):
        self.assertEqual(protocol.selector("f3:dos-conformance run=aB0123ef guest=ciukidos"), ("aB0123ef", "ciukidos"))
        self.assertEqual(protocol.selector("f3:dos-conformance run=aB0123ef guest=ciukidos kernel=unified"), ("aB0123ef", "ciukidos"))
        for value in ("", "f3:dos-conformance", "f3:dos-conformance run=1234567 guest=freedos",
                      "f3:dos-conformance run=12345678 guest=master", "f3:dos-conformance run=12345678 guest=freedos ",
                      "f3:dos-conformance run=12345678 guest=freedos guest=ciukidos",
                      "f3:dos-conformance run=12345678 guest=freedos unknown=1"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                protocol.selector(value)

    def test_complete(self):
        self.assertEqual(len(self.parse(self.lines())), len(conformance.tags()))

    def test_order_partial_stale_teardown(self):
        good = self.lines()
        variants = [good[:-1], good[1:], good[:2]+good[3:], good[:2]+good[1:], good[:1]+[good[2],good[1]]+good[3:],
                    [good[0].replace("12345678", "deadbeef")]+good[1:],
                    good[:-2]+good[-1:], good[:-2]+[good[-2].replace("locks=0", "locks=1"),good[-1]],
                    good+[good[-1]], good+[good[1]]]
        for value in variants:
            with self.subTest(value=value[-2:]), self.assertRaises(ValueError):
                self.parse(value)

    def test_envelope_sequence_run_and_drops(self):
        good = self.lines()
        text = good[1].encode().hex()
        row = "CIUKI_TEST run=12345678 seq=9 event=DATA group=guest part=1 parts=1 data="+text
        self.assertEqual(self.parse([good[0],row]+good[2:]), self.parse(good))
        for bad in (row.replace("12345678", "deadbeef"), row+" dropped=1", row+" run=12345678",
                    row.replace("seq=9 ", ""), row.replace(text, "ff")):
            with self.subTest(bad=bad), self.assertRaises((ValueError, UnicodeError)):
                self.parse([good[0],bad]+good[2:])
        with self.assertRaises(ValueError):
            self.parse([good[0],row,"CIUKI_TEST run=12345678 seq=8 event=DATA group=trace"]+good[2:])

    def test_identical_unsupported_results_fail(self):
        lines = [line.replace("PASS 0000", "FAIL 0001") for line in self.lines()]
        result = self.parse(lines)
        self.assertFalse(conformance.compare(result,result)[1])

    def test_probe_manifest_rejects_helpers_and_changed_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            folder=Path(directory)
            manifest=conformance.build(folder)
            protocol.probe_manifest(manifest, lambda name:(folder/name).read_bytes())
            for key in ("cases", "helpers"):
                wrong=copy.deepcopy(manifest); wrong[key]=wrong[key][1:]
                with self.assertRaises(ValueError): protocol.probe_manifest(wrong)
            (folder/manifest["helpers"][0]["binary"]).write_bytes(b"corrupt")
            with self.assertRaises(ValueError):
                protocol.probe_manifest(manifest, lambda name:(folder/name).read_bytes())


class ProductionParserTests(unittest.TestCase):
    def test_c_parser(self):
        import subprocess
        import os
        root=conformance.ROOT
        parent=root/"build/host/dos"; parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as directory:
            binary=Path(directory)/"selector"
            subprocess.run(["clang","-std=c17","-O1","-g","-Wall","-Wextra","-Werror",
                            "-fsanitize=address,undefined","-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
                            "-I",str(root/"src/kernel/include"),str(root/"tests/host/dos/selector_test.c"),
                            str(root/"src/kernel/probes/selector.c"),
                            str(root/"tests/host/dos/loader_selector_test.c"),"-lx86emu","-o",str(binary)],check=True)
            # Keep this existing parser test's sanitizer settings for every
            # invocation of the same test binary.
            parser_env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0")
            subprocess.run([str(binary)],check=True,env=parser_env)
            # Assemble bounded, unchanged production code/data ranges. The
            # wrapper only sets inputs and returns CF through a host interrupt.
            source=(root/"src/boot/ciukldr/menu.inc").read_text()
            validate=source.split("selector_accept:",1)[0]
            data=source.split("chain_probe_names db",1)[1].split("config_read:",1)[0]
            data+='\nf4_probe_names db'+source.split('\nf4_probe_names db',1)[1]
            assembly=Path(directory)/"loader.asm"
            assembly.write_text('bits 16\norg 0x1000\njmp near start\n'
                'request_len dw 0\nrequest_source db 0\nrequest times 65 db 0\n'
                'start:\ncld\nmov si,request\nmov cx,[request_len]\n'
                'mov al,[request_source]\nmov [selector_fw],al\ncall selector_validate\n'
                'mov ax,0\nadc ax,0\nint 0xe9\nhlt\n'+validate+'\nchain_probe_names db'+data)
            loader=Path(directory)/"loader.bin"
            subprocess.run(["nasm","-f","bin",str(assembly),"-o",str(loader)],check=True)
            import importlib.util
            spec=importlib.util.spec_from_file_location("h55_loader_model",root/"scripts/test/loader_model.py")
            model=importlib.util.module_from_spec(spec);spec.loader.exec_module(model)
            base="f3:dos-conformance run=1234aBcD"
            valid=[base+" guest=freedos",base+" guest=ciukidos",base+" guest=ciukidos kernel=unified"]
            invalid=[base,base+" guest=unified",base+" guest=freedos kernel=unified",
                     base+" guest=ciukidos kernel=master",base+" guest=ciukidos kernel=unified ",
                     base+" guest=freedos guest=ciukidos",base+" guest=freedos platform=e500",
                     base+" guest=ciukidos safe=1",base+" guest=ciukidos kernel=unified x=1",
                     base.replace("1234aBcD","1234aBcG")+" guest=ciukidos"]
            for text in valid+invalid:
                accepted=text in valid
                with self.subTest(selector=text):
                    if accepted:
                        self.assertEqual(protocol.selector(text),("1234aBcD", "freedos" if text.endswith("freedos") else "ciukidos"))
                        self.assertEqual(model.selector(text)["kernel"],"unified" if text.endswith("unified") else text.split("guest=")[1])
                    else:
                        with self.assertRaises(ValueError): protocol.selector(text)
                        with self.assertRaises(ValueError): model.selector(text)
                    for origin in (0,1,2):  # menu, validated fw_cfg, BOOT.CFG
                        subprocess.run([str(binary),str(loader),text,str(origin),str(int(accepted))],check=True,
                                       env=parser_env)
            for probe in model.F3_PROBES:
                if probe == "dos-conformance": continue
                text=f"f3:{probe} run=1234aBcD"
                self.assertEqual(model.selector(text)["probe"],probe)
                for origin in (0,1,2):
                    subprocess.run([str(binary),str(loader),text,str(origin),"1"],check=True,
                                   env=parser_env)
                override=text+" platform=e500"
                self.assertEqual(model.selector(override,"fw_cfg",True)["platform"],"e500")
                for origin in (0,1,2):
                    subprocess.run([str(binary),str(loader),override,str(origin),str(int(origin==1))],check=True,
                                   env=parser_env)

class RunnerOwnershipTests(unittest.TestCase):
    def test_two_independent_pairs_and_overlay_cleanup(self):
        import contextlib
        import qemu_reference as runner
        from unittest.mock import patch, Mock
        import subprocess
        import sys
        root=conformance.ROOT
        profile=(root/"tests/profiles/qemu-t23.json").read_text()
        for failure in (None,"capture","create"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as d:
                folder=Path(d); image=folder/"base"; image.write_bytes(b"not an image; mocked read-back only")
                probes=folder/"probes";probes.write_text("{}")
                identity=folder/"identity";identity.write_text("{}")
                (folder/"tests/profiles").mkdir(parents=True)
                (folder/"tests/profiles/qemu-t23.json").write_text(profile)
                rows=conformance.records("\n".join(t+" PASS 0000" for t in conformance.tags()))
                created=[]
                def create(argv,**kw):
                    path=Path(argv[-1]);path.write_bytes(b"mock COW");created.append(path)
                    if failure=="create": raise subprocess.CalledProcessError(1,argv)
                    return subprocess.CompletedProcess(argv,0)
                capture=Mock(return_value=rows)
                if failure=="capture":capture.side_effect=ValueError("mock partial log")
                args=["runner","--image",str(image),"--integration-manifest",str(identity),"--probes-manifest",str(probes)]
                with patch.object(runner,"ROOT",folder),patch.object(sys,"argv",args), \
                     patch.object(runner,"qualified",return_value={"image_sha256":conformance.digest(image)}), \
                     patch.object(runner,"capped"),patch.object(runner.subprocess,"run",side_effect=create), \
                     patch.object(runner,"capture",capture), \
                     patch.object(runner,"reconcile_overlay",return_value={"records":1}), \
                     patch.object(runner.res,"ExclusiveLock",side_effect=lambda _:contextlib.nullcontext()), \
                     patch.object(runner.res,"slot_paths",return_value=[]), \
                     patch.object(runner.res,"common_lock",return_value=folder/"mock-lock"):
                    if failure:
                        with self.assertRaises((ValueError,subprocess.CalledProcessError)): runner.main()
                    else:
                        self.assertEqual(runner.main(),0)
                        self.assertEqual(len(created),4)
                        calls=capture.call_args_list
                        self.assertNotEqual(calls[0].args[2],calls[2].args[2])
                        self.assertEqual([c.args[3] for c in calls],["freedos","ciukidos"]*2)
                self.assertFalse(any(p.exists() for p in created))

    def test_setup_failure_and_forced_kill_reap(self):
        import qemu_reference as runner
        from unittest.mock import patch, Mock
        import subprocess
        for setup_failure in (True,False):
            proc=Mock();proc.poll.return_value=None
            poller=Mock()
            proc.wait.side_effect=[subprocess.TimeoutExpired("mock",5),0]
            with tempfile.TemporaryDirectory() as d, \
                 patch.object(runner.subprocess,"Popen",return_value=proc), \
                 patch.object(runner.selectors,"DefaultSelector",side_effect=OSError("mock selector") if setup_failure else None,return_value=poller), \
                 patch.object(runner.time,"monotonic",side_effect=[0,1000]):
                path=Path(d)/"log"
                with self.assertRaises((OSError,ValueError)):runner.capture(["never-executed"],path,"12345678","freedos",900)
                proc.terminate.assert_called_once();proc.kill.assert_called_once();self.assertEqual(proc.wait.call_count,2)
                proc.stdout.close.assert_called_once();self.assertEqual(path.read_bytes(),b"")

class ProductionControllerTests(unittest.TestCase):
    def test_c_controller(self):
        import subprocess
        import os
        root=conformance.ROOT
        parent=root/"build/host/dos";parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as directory:
            binary=Path(directory)/"controller"
            subprocess.run(["clang","-std=c17","-O1","-g","-Wall","-Wextra","-Werror",
                            "-Wno-pointer-to-int-cast","-fsanitize=address,undefined",
                            "-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
                            "-I",str(root/"src/kernel/include"),str(root/"tests/host/dos/controller_test.c"),
                            "-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"))

if __name__ == "__main__": unittest.main()

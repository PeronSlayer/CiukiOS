import copy
import json
from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
import loader_model as model


class LoaderModelTests(unittest.TestCase):
    def test_cpu_minimum_features_and_signatures(self):
        for signature,expected in ((0x652,(6,5,2)), (0x623,(6,2,3)),
                                   (0xf29,(15,2,9)), (0x000106a5,(6,26,5)),
                                   (0x00230f12,(17,49,2))):
            with self.subTest(signature=hex(signature)):
                result=model.cpu_minimum(True,1,signature,1<<15)
                self.assertEqual((result['family'],result['model'],result['stepping']),expected)
                self.assertEqual(result['signature'],signature)
        # No FXSR or SSE is necessary; neither compensates for absent CMOV.
        for present,leaf,features in ((False,1,1<<15),(True,0,1<<15),
                                     (True,1,0),(True,1,(1<<24)|(1<<25))):
            with self.subTest(present=present,leaf=leaf,features=features):
                with self.assertRaisesRegex(ValueError,'i686 CPU with CMOV'):
                    model.cpu_minimum(present,leaf,0x543,features)

    def test_pre_disk_firmware_minimum_and_recovery(self):
        cpu=dict(cpuid_present=True,max_basic_leaf=1,signature=0x652,features_edx=1<<15)
        memory=[dict(base=0x100000,length=127*1024**2,type=1)]
        for version in (0x200,0x300):
            result=model.boot_minimum(cpu,memory,dict(signature='VESA',version=version))
            self.assertTrue(result['vbe_present'])
            self.assertEqual(result['memory'],model.normalize_e820(memory))
        for controller in (None,dict(query_ax=0x14f,signature='VESA',version=0x100)):
            self.assertFalse(model.boot_minimum(cpu,memory,controller)['vbe_present'])
        for controller in (dict(signature='VESA',version=0x102),dict(signature='bad',version=0x300)):
            with self.assertRaisesRegex(ValueError,'VBE 2.0'):
                model.boot_minimum(cpu,memory,controller)
        for entries,options in (([],{}),(memory,{'complete':False}),(memory,{'signature':'bad'}),
                                ([dict(base=2**64-1,length=2,type=1)],{})):
            with self.assertRaisesRegex(ValueError,'valid BIOS E820'):
                model.boot_minimum(cpu,entries,**options)
        # Refusal order matches assembly: no VBE/disk work after a CPU/map failure.
        with self.assertRaisesRegex(ValueError,'i686 CPU'):
            model.boot_minimum(dict(cpuid_present=False),[],dict(version=0x100))

    def test_production_minimum_gate_precedes_disk_and_halts(self):
        loader=(ROOT/'src/boot/ciukldr.asm').read_text()
        entry=loader.split('entry:',1)[1].split('; All errors',1)[0]
        calls=['call uart_init','call cpu_minimum','call memory_collect','call vbe_minimum',
               'call platform_collect','call fat_mount','call config_read','call elf_load']
        self.assertEqual([entry.index(c) for c in calls],sorted(entry.index(c) for c in calls))
        platform=(ROOT/'src/boot/ciukldr/platform.inc').read_text()
        halt=platform.split('minimum_halt:',1)[1].split('; Optional firmware',1)[0]
        self.assertIn('cli',halt);self.assertIn('hlt',halt)
        self.assertNotIn('int 0x16',halt);self.assertNotIn('int 0x13',halt)
        for message in (model.CPU_MINIMUM_ERROR,model.VBE_MINIMUM_ERROR):
            self.assertIn(message,platform)
        self.assertIn(model.E820_MINIMUM_ERROR,(ROOT/'src/boot/ciukldr/memory.inc').read_text())

    def test_menu_safe_sources_are_monotonic(self):
        config = model.boot_options('safe=1 safe=0')
        self.assertTrue(model.menu_choice('N', config['safe']))
        fw_cfg = model.selector('f1:safe run=12345678 safe=1', 'fw_cfg', True)
        self.assertTrue(model.menu_choice('n', fw_cfg['safe']))
        self.assertTrue(model.menu_choice('S'))
        self.assertFalse(model.menu_choice('N'))

    def test_hardware_replay(self):
        for machine in ('t23','e500'):
            data=json.loads((ROOT/'tests/fixtures/hardware'/machine/'replay.json').read_text())
            self.assertEqual(model.input_policy(data['pci'],data['pci_bios_ok']),data['expected_input_policy'])
            for memory in data['memory_maps']:
                self.assertEqual(model.normalize_e820(memory['entries']),memory['expected']['normalized'])
                self.assertEqual(sum((b-a)//4096 for a,b in model.page_candidates(memory['entries'])),memory['expected']['candidate_pages'])
            for video in data['video_sets']:
                for safe,key in ((False,'normal'),(True,'safe')):
                    self.assertEqual(model.select_video(video['controller'],video['modes'],safe=safe),video['expected'][key])

    def test_t23_malformed_scanline_is_not_repaired(self):
        data=json.loads((ROOT/'tests/fixtures/hardware/t23/replay.json').read_text())
        bad=data['video_sets'][0]
        self.assertEqual(model.mode_eligibility(bad['controller'],bad['modes'][0]),'scanline')
        self.assertTrue(model.select_video(bad['controller'],bad['modes'])['text'])

    def test_twenty_byte_ext_and_overlap_rounding(self):
        records=[dict(base=1,length=0x6fff,type=1,record_size=20,ext=0),dict(base=0x2800,length=0x100,type=4)]
        self.assertEqual(model.normalize_e820(records),[
            dict(base=0x1000,length=0x1000,type=1,ext=1),dict(base=0x2800,length=0x100,type=4,ext=1),dict(base=0x3000,length=0x4000,type=1,ext=1)])
        # Splitting the same usable span at unaligned overlap boundaries must
        # not discard a complete page before inward rounding.
        records=[dict(base=0,length=8192,type=1),dict(base=17,length=3000,type=1)]
        self.assertEqual(model.normalize_e820(records),[dict(base=0,length=8192,type=1,ext=1)])

    def test_e820_rejection_and_disabled_entries(self):
        good=dict(base=0x100000,length=0x10000,type=1)
        for records in ([],[dict(base=2**64-1,length=2,type=1)],[dict(base=0,length=0,type=1)],[dict(base=0,length=4096,type=1,record_size=21)],[dict(base=0,length=4096,type=1,ext=0)],[good]*129):
            with self.subTest(records=records[:1]),self.assertRaises(ValueError):model.normalize_e820(records)
        with self.assertRaises(ValueError):model.normalize_e820([good],complete=False)
        self.assertEqual(model.normalize_e820([good,dict(base=0,length=4096,type=1,ext=0)]),[dict(**good,ext=1)])

    def test_unknown_type_is_reserved_and_clipped_allocations(self):
        records=[dict(base=0,length=1024**3,type=1),dict(base=0x200000,length=4096,type=99)]
        normalized=model.normalize_e820(records)
        self.assertEqual(normalized[1]['type'],99)
        ranges=model.page_candidates(records)
        self.assertEqual(ranges[0][0],0x100000);self.assertEqual(ranges[-1][1],768*1024**2)

    def video(self):
        data=json.loads((ROOT/'tests/fixtures/hardware/t23/replay.json').read_text())
        return copy.deepcopy(data['video_sets'][1])

    def test_vbe_version_masks_capacity_address_and_flags(self):
        data=self.video();c=data['controller'];m=data['modes'][0]
        m['linear_pitch']=1
        self.assertIsNone(model.mode_eligibility(c,m)) # VBE2 ignores linear fields
        c['version']=0x300;self.assertEqual(model.mode_eligibility(c,m),'scanline')
        m['linear_pitch']=4096;m['linear_masks']=copy.deepcopy(m['masks'])
        self.assertIsNone(model.mode_eligibility(c,m))
        for key,value,reason in [('physical_base',0,'address'),('physical_base',0xfffff000,'address'),('attributes',0x11,'attributes'),('memory_model',4,'geometry_or_memory_model')]:
            bad=copy.deepcopy(m);bad[key]=value;self.assertEqual(model.mode_eligibility(c,bad),reason)
        bad=copy.deepcopy(m);bad['linear_masks']['green']=[8,16];self.assertEqual(model.mode_eligibility(c,bad),'masks')
        c['total_memory_64k']=1;self.assertEqual(model.mode_eligibility(c,m),'capacity')

    def test_preference_safe_and_readback_failure(self):
        d=self.video();c=d['controller'];m=d['modes'][0]
        small=copy.deepcopy(m);small.update(mode=0x112,width=640,height=480,banked_pitch=2560)
        self.assertEqual(model.select_video(c,[small,m])['mode'],m['mode'])
        self.assertEqual(model.select_video(c,[small,m],configured=small['mode'])['mode'],small['mode'])
        self.assertEqual(model.select_video(c,[small,m],safe=True)['mode'],small['mode'])
        self.assertEqual((m['width'],m['height']),(1024,768))
        self.assertEqual(model.select_video(c,[small,m],configured=m['mode'],safe=True)['mode'],small['mode'])
        m['readback_mode']=0x115;self.assertEqual(model.select_video(c,[small,m])['mode'],small['mode'])
        small['set_ok']=False;self.assertTrue(model.select_video(c,[small,m])['text'])

    def test_selector_trust_and_input_quirk(self):
        for bad in ('f0:nope run=12345678','f0:boot run=1234','f0:boot  run=12345678','f0:boot run=12345678\n','f0:boot run=12345678 platform=t23','é'*65):
            with self.assertRaises(ValueError):model.selector(bad)
        override='f0:boot run=12ab34cd platform=e500'
        with self.assertRaises(ValueError):model.selector(override)
        self.assertTrue(model.input_policy([],request=override,validated_fw_cfg=True)['forced'])
        d=json.loads((ROOT/'tests/fixtures/hardware/e500/replay.json').read_text())
        self.assertEqual(model.input_policy(d['pci'])['policy'],1)
        self.assertEqual(model.input_policy(d['pci'],False)['policy'],0)
        d['pci'][1]['subsystem_device']=0;self.assertEqual(model.input_policy(d['pci'])['policy'],0)

"""No execution claims: decompression and static edges in the pinned corpus."""
import json
import unittest
import inventory
import reachability

class InventoryTests(unittest.TestCase):
    def test_pinned_twelve_file_audit_reproduces(self):
        checked=json.loads((inventory.ROOT/"docs/validation/f3-48-static-candidates.json").read_text())
        self.assertEqual(inventory.collect(),checked)

    def test_actual_lzexe_decompression_and_static_edges(self):
        result=reachability.collect()
        self.assertEqual(len(result["files"]),12)
        packed=[r for r in result["files"] if r["format"]=="LZEXE"]
        self.assertEqual(len(packed),7)
        wolf=next(r for r in packed if r["path"]=="/WOLF3D/WOLF3D.EXE")
        self.assertGreater(wolf["load_bytes"],109959)
        self.assertGreater(wolf["static"]["instructions"],0)
        self.assertEqual(result["runtime_trace"],"not-collected")
        self.assertFalse(any(r["format"]=="DOS/32A" for r in result["files"]))
        for row in packed:
            for call in row["static"]["interrupts"]:
                self.assertEqual(call["evidence"],"static-direct-reachability")

    def test_bounds_reject_non_lzexe(self):
        for data in (b"",bytes(64),b"MZ"+bytes(26)+b"LZ91"):
            with self.assertRaises(ValueError): reachability.lzexe(data)

    def test_direct_edges_exclude_unreachable_byte_pattern(self):
        # jmp over MOV AH,FF; INT21 (data/unexecuted), to MOV AH,30; INT21; RET.
        data=bytes.fromhex("eb04b4ffcd21b430cd21c3")
        result=reachability.reachable(data,0)
        self.assertEqual([(r["offset"],r["ax"]) for r in result["interrupts"]],[(8,"30??")])
        self.assertFalse(reachability.reachable(bytes.fromhex("ffe0"),0)["complete"])

if __name__=="__main__":unittest.main()

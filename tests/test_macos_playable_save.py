"""The debug profile bypasses only the opening cutscene in a copied SRAM."""

import unittest

from tools.make_macos_playable_test_save import patch_opening_cutscene, save_checksum


class PlayableSaveTest(unittest.TestCase):
    def make_sram(self):
        sram = bytearray(0x8000)
        for offset in (0x20, 0x3D10):
            sram[offset:offset + 4] = (0xBB).to_bytes(4, "big")
            sram[offset + 4:offset + 8] = (1).to_bytes(4, "big")
            sram[offset + 8:offset + 12] = (0xFFF1).to_bytes(4, "big")
            sram[offset + 0x1352:offset + 0x1354] = save_checksum(
                sram[offset:offset + 0x1354]).to_bytes(2, "big")
        return sram

    def test_only_cutscene_and_checksum_change_in_both_slot_copies(self):
        before = self.make_sram()
        after = patch_opening_cutscene(before)
        self.assertEqual(before[0x28:0x2C], (0xFFF1).to_bytes(4, "big"))
        for offset in (0x20, 0x3D10):
            self.assertEqual(after[offset + 8:offset + 12], bytes(4))
            stored = int.from_bytes(after[offset + 0x1352:offset + 0x1354], "big")
            self.assertEqual(stored, save_checksum(after[offset:offset + 0x1354]))
        changed = {i for i, (old, new) in enumerate(zip(before, after)) if old != new}
        allowed = {offset + i for offset in (0x20, 0x3D10)
                   for i in (*range(8, 12), 0x1352, 0x1353)}
        self.assertLessEqual(changed, allowed)
        self.assertTrue(changed)

    def test_refuses_bad_checksum_and_other_cutscene(self):
        broken = self.make_sram()
        broken[0x40] ^= 1
        with self.assertRaises(ValueError):
            patch_opening_cutscene(broken)
        other = self.make_sram()
        other[0x28:0x2C] = bytes(4)
        with self.assertRaises(ValueError):
            patch_opening_cutscene(other)

    def test_optional_saria_greeting_flag_is_set_only_in_copied_slots(self):
        before = self.make_sram()
        after = patch_opening_cutscene(before, skip_saria_greeting=True)
        for offset in (0x20, 0x3D10):
            self.assertEqual(int.from_bytes(after[offset + 0xEF8:offset + 0xEFA], "big"), 1)
            self.assertEqual(int.from_bytes(after[offset + 0x1352:offset + 0x1354], "big"),
                             save_checksum(after[offset:offset + 0x1354]))
        changed = {i for i, (old, new) in enumerate(zip(before, after)) if old != new}
        allowed = {offset + i for offset in (0x20, 0x3D10)
                   for i in (*range(8, 12), 0xEF8, 0xEF9, 0x1352, 0x1353)}
        self.assertLessEqual(changed, allowed)


if __name__ == "__main__":
    unittest.main()

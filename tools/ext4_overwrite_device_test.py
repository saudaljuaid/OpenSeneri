#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check the overwrite command oracle against exact block states and corruption."""
from pathlib import Path
import tempfile
import unittest

from ext4_unlink_powercut_test import verify_overwrite_device_image


class OverwriteDeviceTests(unittest.TestCase):
    def test_each_completed_block_combination_and_unrelated_commands(self):
        with tempfile.TemporaryDirectory() as raw:
            image = Path(raw) / "disk.raw"
            for mask in range(4):
                with self.subTest(mask=mask):
                    content = bytearray((b"t" * 4500).ljust(8192, b"\0"))
                    commands = [("1", "write", "1"), ("2", "flush", "0")]
                    for index, start, end in ((0, 123, 4096), (1, 4096, 4220)):
                        if mask & (1 << index):
                            content[start:end] = b"s" * (end - start)
                            commands.append((str(len(commands) + 1), "write", str((5, 2)[index])))
                    disk = bytearray(6 * 4096)
                    disk[5 * 4096:6 * 4096] = content[:4096]
                    disk[2 * 4096:3 * 4096] = content[4096:]
                    image.write_bytes(disk)
                    self.assertEqual(verify_overwrite_device_image(image, [5, 2], commands), mask)
                    # Contents, untouched prefix/suffix and bytes beyond EOF all matter.
                    for offset in (5 * 4096, 5 * 4096 + 512, 2 * 4096 + 300, 2 * 4096 + 700):
                        changed = bytearray(disk)
                        changed[offset] ^= 1
                        image.write_bytes(changed)
                        with self.assertRaisesRegex(RuntimeError, "exact completed data commands"):
                            verify_overwrite_device_image(image, [5, 2], commands)

    def test_repeated_data_commands_and_invalid_mapping_are_refused(self):
        with tempfile.TemporaryDirectory() as raw:
            image = Path(raw) / "absent.raw"
            for mapping in ([0, 2], [2, 2], [2], [2, 3, 4]):
                with self.assertRaisesRegex(RuntimeError, "two distinct"):
                    verify_overwrite_device_image(image, mapping, [])
            with self.assertRaisesRegex(RuntimeError, "more than once"):
                verify_overwrite_device_image(image, [5, 2], [("1", "write", "5"), ("2", "write", "5")])


if __name__ == "__main__":
    unittest.main()

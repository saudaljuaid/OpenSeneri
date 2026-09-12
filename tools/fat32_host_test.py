#!/usr/bin/env python3
"""Positive and adversarial tests for the deterministic FAT32 host tool."""

from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile
import unittest

import fat32_image as fat32


class Fat32HostTests(unittest.TestCase):
    def setUp(self) -> None:
        self.image = fat32.build_image("data")

    def test_deterministic_data_format(self) -> None:
        rebuilt = fat32.build_image("data")
        self.assertEqual(self.image, rebuilt)
        report = fat32.verify_data(self.image)
        self.assertEqual(report["format"], "FAT32")
        self.assertEqual(report["fat_copies"], 2)
        self.assertEqual(report["volume_id"], "20000002")
        self.assertEqual(report["files"], [])
        self.assertGreaterEqual(report["cluster_count"], fat32.FAT32_MIN_CLUSTERS)

    def test_atomic_output_is_byte_identical(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "data.raw"
            fat32.atomic_write(output, self.image)
            self.assertEqual(output.read_bytes(), self.image)
            self.assertEqual(
                hashlib.sha256(output.read_bytes()).digest(),
                hashlib.sha256(fat32.build_image("data")).digest(),
            )

    def test_full_fixture_is_consistent_and_reconstructable(self) -> None:
        image = fat32.build_full_data_image()
        report = fat32.verify_full_data(image)
        self.assertEqual(report["free_clusters"], 0)
        self.assertEqual(len(report["files"]), 5)
        self.assertEqual(image, fat32.build_full_data_image())

    def test_host_staging_populates_one_bounded_media_file(self) -> None:
        payload = bytes(range(256)) * 5
        populated = fat32.populate_data_image(
            self.image, "AURORA.BMP", payload
        )
        report = fat32.inspect_image(populated)
        self.assertEqual(
            report["files"],
            [{"path": "AURORA.BMP", "directory": False,
              "size": len(payload), "first_cluster": 3}],
        )
        self.assertEqual(
            populated,
            fat32.populate_data_image(self.image, "AURORA.BMP", payload),
        )
        with self.assertRaisesRegex(fat32.Fat32Error, "8.3"):
            fat32.populate_data_image(self.image, "TOO-LONG-NAME.BMP", payload)

    def test_host_staging_populates_a_bounded_file_set(self) -> None:
        image = fat32.populate_data_files(self.image, [
            ("AURORA.BMP", bytes(range(256)) * 5),
            ("NEW1.TXT", b"Phipia note"),
        ])
        report = fat32.inspect_image(image)
        self.assertEqual(
            [(item["path"], item["first_cluster"]) for item in report["files"]],
            [("AURORA.BMP", 3), ("NEW1.TXT", 6)],
        )
        self.assertTrue(report["fat_copies_match"])
        self.assertEqual(report["cycles"], 0)
        self.assertEqual(report["cross_links"], 0)
        self.assertEqual(report["leaked_clusters"], 0)
        with self.assertRaisesRegex(fat32.Fat32Error, "unique"):
            fat32.populate_data_files(self.image, [
                ("SAME.TXT", b"one"), ("same.txt", b"two")
            ])

    def test_host_staging_populates_application_namespaces(self) -> None:
        files = [
            ("LUA/SCRIPT.LUA", b"print('Phipia')\n"),
            ("SQLITE/SEED.TXT", b"seed"),
            ("LUA/INPUT.TXT", b"phipia\n"),
        ]
        populated = fat32.populate_data_tree(self.image, files)
        report = fat32.inspect_image(populated)
        ordinary = [item for item in report["files"]
                    if not item["directory"] and not item["path"].endswith("/.")
                    and not item["path"].endswith("/..")]
        self.assertEqual(
            [(item["path"], item["size"]) for item in ordinary],
            [("LUA/INPUT.TXT", 7), ("LUA/SCRIPT.LUA", 16),
             ("SQLITE/SEED.TXT", 4)],
        )
        self.assertEqual(populated, fat32.populate_data_tree(
            self.image, list(reversed(files))))
        with self.assertRaisesRegex(fat32.Fat32Error, "DIRECTORY/NAME"):
            fat32.populate_data_tree(self.image, [("ROOT.TXT", b"bad")])

    def test_all_supported_malformed_images_are_detected(self) -> None:
        kinds = (
            "boot-signature", "bpb-sector-size", "bpb-cluster-size",
            "bpb-overflow", "root-cluster", "fsinfo-signature", "fsinfo-hint",
            "fat-mismatch", "reserved-cluster", "bad-cluster", "out-of-range",
            "cycle", "truncated-directory", "malformed-lfn",
        )
        for kind in kinds:
            with self.subTest(kind=kind):
                malformed = fat32.mutate_image(kind, self.image)
                with self.assertRaises(fat32.Fat32Error):
                    fat32.inspect_image(malformed)

    def test_checked_arithmetic_rejects_overflow(self) -> None:
        with self.assertRaises(fat32.Fat32Error):
            fat32.checked_add((1 << 64) - 1, 1)
        with self.assertRaises(fat32.Fat32Error):
            fat32.checked_mul(1 << 63, 2)

    def test_invalid_fsinfo_is_never_authoritative(self) -> None:
        geometry = fat32.parse_geometry(self.image)
        changed = bytearray(self.image)
        offset = geometry.sector_offset(geometry.fsinfo_sector) + 488
        fat32.put_u32(changed, offset, geometry.cluster_count)
        with self.assertRaisesRegex(fat32.Fat32Error, "FSInfo"):
            fat32.inspect_image(bytes(changed))

    def test_fat_cycle_and_cross_link_are_bounded(self) -> None:
        geometry = fat32.parse_geometry(self.image)
        changed = bytearray(self.image)
        root = geometry.sector_offset(geometry.cluster_sector(geometry.root_cluster))
        first_file = fat32._directory_entry(b"ONE     TXT", 0x20, 3, 1)
        second_file = fat32._directory_entry(b"TWO     TXT", 0x20, 3, 1)
        changed[root + 32:root + 64] = first_file
        changed[root + 64:root + 96] = second_file
        for copy in range(geometry.fat_copies):
            offset = geometry.sector_offset(
                geometry.first_fat_sector + copy * geometry.fat_sectors
            ) + 3 * 4
            fat32.put_u32(changed, offset, fat32.FAT32_EOC)
        with self.assertRaisesRegex(fat32.Fat32Error, "cross-linked"):
            fat32.inspect_image(bytes(changed))

    def test_duplicate_ascii_casefold_name_is_rejected(self) -> None:
        geometry = fat32.parse_geometry(self.image)
        changed = bytearray(self.image)
        root = geometry.sector_offset(geometry.cluster_sector(geometry.root_cluster))
        changed[root + 32:root + 64] = fat32._directory_entry(
            b"NOTES   TXT", 0x20, 0, 0
        )
        changed[root + 64:root + 96] = fat32._directory_entry(
            b"notes   txt", 0x20, 0, 0
        )
        with self.assertRaisesRegex(fat32.Fat32Error, "duplicate"):
            fat32.inspect_image(bytes(changed))

    def test_identical_names_in_separate_directories_are_scoped(self) -> None:
        geometry = fat32.parse_geometry(self.image)
        changed = bytearray(self.image)
        root = geometry.sector_offset(
            geometry.cluster_sector(geometry.root_cluster)
        )
        changed[root + 32:root + 64] = fat32._directory_entry(
            b"FIRST      ", 0x10, 3, 0
        )
        changed[root + 64:root + 96] = fat32._directory_entry(
            b"SECOND     ", 0x10, 4, 0
        )
        for cluster in (3, 4):
            offset = geometry.sector_offset(geometry.cluster_sector(cluster))
            changed[offset:offset + 32] = fat32._directory_entry(
                b".          ", 0x10, cluster, 0
            )
            changed[offset + 32:offset + 64] = fat32._directory_entry(
                b"..         ", 0x10, geometry.root_cluster, 0
            )
            changed[offset + 64:offset + 96] = fat32._directory_entry(
                b"NOTES   TXT", 0x20, 0, 0
            )
            for copy in range(geometry.fat_copies):
                fat_offset = geometry.sector_offset(
                    geometry.first_fat_sector +
                    copy * geometry.fat_sectors
                ) + cluster * 4
                fat32.put_u32(changed, fat_offset, fat32.FAT32_EOC)
        for sector in (
            geometry.fsinfo_sector,
            geometry.backup_boot_sector + geometry.fsinfo_sector,
        ):
            info = geometry.sector_offset(sector)
            fat32.put_u32(
                changed, info + 488, geometry.cluster_count - 3
            )
            fat32.put_u32(changed, info + 492, 5)
        report = fat32.inspect_image(bytes(changed))
        note_paths = [
            item["path"] for item in report["files"]
            if item["path"].endswith("/NOTES.TXT")
        ]
        self.assertEqual(
            note_paths,
            ["FIRST/NOTES.TXT", "SECOND/NOTES.TXT"],
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)

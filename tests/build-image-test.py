#!/usr/bin/env python3
"""Check carrier geometry and reject oversized roots before creating output."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('builder', Path(__file__).resolve().parents[1] / 'storage/build-image.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
CAPACITY = 128 * 1024 * 1024

class ImageTests(unittest.TestCase):
    def inputs(self, directory, profile, size=4096):
        boot, root = directory / 'boot', directory / 'root'
        with boot.open('wb') as dest:
            dest.truncate((builder.LAYOUTS[profile][2] * 512) if profile == 'pro-emmc' else 36 * 1024 * 1024)
        with root.open('wb') as dest:
            if size:
                header = bytearray(48)
                header[:4] = b'hsqs'
                struct.pack_into('<Q', header, 40, size)
                dest.write(header)
            dest.truncate(size)
        return boot, root

    def test_all_media_have_128_mib_and_preserve_cube_alignment(self):
        expected = {'nanokvm': (264192, 528384, 790528),
                    'pro-sd': (264192, 526336, 788480),
                    'pro-emmc': (568832, 830976, 1093120)}
        for profile, offsets in expected.items():
            with self.subTest(profile=profile):
                _, _, _, a, count, b, data = builder.LAYOUTS[profile]
                self.assertEqual(builder.LAYOUTS[profile][2] * 512, CAPACITY)
                self.assertEqual(count * 512, CAPACITY)
                self.assertEqual((a, b, data), offsets)
                self.assertEqual((b - a, data - b), (count + (2048 if profile == 'nanokvm' else 0), count))

    def test_carrier_manifest_and_partition_table(self):
        for profile in builder.LAYOUTS:
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as work:
                directory = Path(work)
                boot, root = self.inputs(directory, profile)
                output = directory / 'image'
                builder.build(profile, boot, root, output, 'test')
                manifest = dict(line.split('=', 1) for line in Path(str(output) + '.manifest').read_text().splitlines())
                self.assertEqual(int(manifest['file_size']), output.stat().st_size)
                self.assertEqual(int(manifest['trailer_offset']), int(manifest['image_size']))
                self.assertEqual(int(manifest['trailer_size']), builder.TRAILER_SIZE)
                with output.open('rb') as source:
                    source.seek(-builder.TRAILER_SIZE, 2)
                    trailer = source.read(builder.TRAILER_SIZE)
                self.assertEqual(trailer[:16], builder.TRAILER_MAGIC)
                self.assertEqual(struct.unpack_from('<HHBBHQ', trailer, 16),
                                 (builder.TRAILER_VERSION, builder.TRAILER_METADATA_SIZE,
                                  builder.PROFILE_IDS[profile], builder.MEDIA_IDS[profile], 0,
                                  int(manifest['image_size'])))
                self.assertEqual(trailer[36:68].hex(), manifest['payload_sha256'])
                self.assertEqual(trailer[-builder.TRAILER_HASH_SIZE:].hex(), manifest['image_sha256'])
                self.assertEqual(manifest['file_sha256'], builder.digest(output))
                self.assertEqual(Path(str(output) + '.sha256').read_text().split()[0],
                                 manifest['file_sha256'])
                self.assertEqual(int(manifest['rootfs_slot_bytes']), CAPACITY)
                self.assertEqual(int(manifest['installed_boot_sectors']) * 512, CAPACITY)
                self.assertEqual(int(manifest['installed_rootfs_a_start_sector']), builder.LAYOUTS[profile][3])
                self.assertEqual(int(manifest['userdata_start_sector']), builder.LAYOUTS[profile][6])
                if profile == 'pro-emmc':
                    self.assertIn('128M@157024256(pro-boot),128M@291241984(pro-rootfs-a),128M(pro-rootfs-b)', manifest['blkdevparts'])
                    with output.open('rb') as source:
                        self.assertEqual(source.read(512), bytes(512))
                else:
                    with output.open('rb') as source:
                        mbr = source.read(512)
                    self.assertEqual(mbr[510:512], b'\x55\xaa')
                    self.assertEqual(struct.unpack_from('<II', mbr, 454),
                                     (builder.LAYOUTS[profile][1], 36 * 2048))
                    source_root = 37 * 2048
                    self.assertEqual(struct.unpack_from('<II', mbr, 470), (source_root, 8))
                    self.assertEqual(output.stat().st_size, source_root * 512 + 4096 + 512)
                    self.assertLess(output.stat().st_size, CAPACITY)

    def test_exact_slot_capacity_is_accepted(self):
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work)
            boot, root = self.inputs(directory, 'nanokvm', CAPACITY)
            builder.build('nanokvm', boot, root, directory / 'image', 'test')

    def test_overflow_and_empty_root_are_rejected_without_output(self):
        for profile in builder.LAYOUTS:
            for size in (0, CAPACITY + 1):
                with self.subTest(profile=profile, size=size), tempfile.TemporaryDirectory() as work:
                    directory = Path(work)
                    boot, root = self.inputs(directory, profile, size)
                    output = directory / 'image'
                    with self.assertRaisesRegex(ValueError, 'rootfs slot capacity'):
                        builder.build(profile, boot, root, output, 'test')
                    self.assertFalse(output.exists())

if __name__ == '__main__':
    unittest.main()

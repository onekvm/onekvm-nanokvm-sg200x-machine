#!/usr/bin/env python3
"""Pack the vendor Pro eMMC layout and check offsets, sparse expansion, and bounds."""
import importlib.util
from io import BytesIO
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location(
    'pack_pro_emmc', Path(__file__).resolve().parents[1] / 'tools/pack-pro-emmc.py')
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)

XML = """<Config>
  <Partitions unit="2">
    <Partition id="spl" size="4" />
    <Partition id="env" size="1" />
    <Partition id="rootfs" size="0xffffffff" />
  </Partitions>
  <ImgList>
    <Img><Block id="spl" /><File>spl.bin</File></Img>
    <Img><Block id="rootfs" /><File>root.simg</File></Img>
    <Img><Block /><File></File></Img>
  </ImgList>
</Config>
"""

FACTORY_OFFSETS = {
    'spl': 0,
    'ddrinit': 786432,
    'atf': 1310720,
    'atf_b': 1572864,
    'uboot': 1835008,
    'uboot_b': 3407872,
    'env': 4980736,
    'logo': 6029312,
    'logo_b': 12320768,
    'optee': 18612224,
    'optee_b': 19660800,
    'dtb': 20709376,
    'dtb_b': 21757952,
    'kernel': 22806528,
    'kernel_b': 89915392,
    'boot': 157024256,
    'rootfs': 291241984,
}


def sparse_image():
    """One raw block, two zero-fill blocks, and one patterned fill block."""
    raw = b'A' * 4096
    chunks = [
        (packer.SPARSE_RAW, 1, raw),
        (packer.SPARSE_FILL, 2, b'\x00\x00\x00\x00'),
        (packer.SPARSE_FILL, 1, b'XYZW'),
    ]
    blob = bytearray(struct.pack('<IHHHHIIII', packer.SPARSE_MAGIC, 1, 0, 28, 12,
                                 4096, 4, len(chunks), 0))
    for kind, count, payload in chunks:
        total = 12 + len(payload)
        blob += struct.pack('<HHII', kind, 0, count, total)
        blob += payload
    return bytes(blob)


class PackTests(unittest.TestCase):
    def test_factory_xml_offsets(self):
        axp = Path(__file__).resolve().parents[2] / 'images/20260529_NanoKVMPro_1_0_15.axp'
        if not axp.is_file():
            self.skipTest('factory axp is not in this workspace')
        with zipfile.ZipFile(axp) as archive:
            xml = archive.read('AX630C_emmc_arm64_k419_sipeed_nanokvm.xml').decode('utf-8')
        parts = packer.parse_partitions(xml)
        self.assertEqual({part.name: part.offset for part in parts}, FACTORY_OFFSETS)
        self.assertIsNone(parts[-1].size)
        self.assertEqual(packer.payload_map(xml)['kernel'], 'boot_signed.bin')
        self.assertNotIn('env', packer.payload_map(xml))

    def test_sparse_root_and_hole_for_missing_partition(self):
        files = {'spl.bin': b'U' * 100, 'root.simg': sparse_image()}
        with tempfile.TemporaryDirectory() as work:
            output = str(Path(work) / 'disk.img')

            def opener(name):
                return BytesIO(files[name])

            _parts, placed, image_end = packer.pack_image(XML, opener, output)
            self.assertEqual(image_end, 5120 + 4 * 4096)
            self.assertEqual([item[2] for item in placed], [100, 16384])
            with open(output, 'rb') as image:
                self.assertEqual(image.read(100), b'U' * 100)
                image.seek(4096)
                self.assertEqual(image.read(1024), bytes(1024))
                image.seek(5120)
                self.assertEqual(image.read(4096), b'A' * 4096)
                image.seek(5120 + 3 * 4096)
                self.assertEqual(image.read(4096), b'XYZW' * 1024)
            self.assertEqual(Path(output).stat().st_size, image_end)

    def test_payload_larger_than_partition_is_rejected(self):
        xml = XML.replace('size="4"', 'size="1"')
        with tempfile.TemporaryDirectory() as work:
            output = str(Path(work) / 'disk.img')
            with self.assertRaises(ValueError):
                packer.pack_image(xml, lambda name: BytesIO(b'U' * 2048 if name == 'spl.bin'
                                                            else sparse_image()), output)

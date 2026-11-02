"""Reject boot inputs that would mix architectures or overwrite resident firmware."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("go_boot", Path(__file__).parents[1] / "package-boot.py")
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


class ImageInputs(unittest.TestCase):
    def run_image(self, *, magic=b"ARM\x64", offset=0, size=64, flags=8):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            image = bytearray(64)
            struct.pack_into("<3Q", image, 8, offset, size, flags)
            image[56:60] = magic
            (root / "Image").write_bytes(image)
            (root / "go.dtb").write_bytes(bytes.fromhex("d00dfeed") + bytes(36))
            argv = ["package-boot", "--sdk", temp, "--kernel", str(root / "Image"),
                    "--dtb", str(root / "go.dtb"), "--out", str(root / "out")]
            # Wrapping is exercised separately by the OE artifact checks.
            with patch.object(sys, "argv", argv), patch.object(boot, "package", return_value={}) as wrap:
                boot.main()
                manifest = json.loads((root / "out/manifest.json").read_text())
                self.assertEqual(manifest["architecture"], "arm64")
                self.assertEqual(wrap.call_count, 2)

    def test_accepts_relocatable_arm64_image(self):
        self.run_image()

    def test_rejects_arm32_or_unrecognized_image(self):
        with self.assertRaises(ValueError):
            self.run_image(magic=bytes(4))

    def test_rejects_image_relocation_over_bl31(self):
        with self.assertRaises(ValueError):
            self.run_image(flags=0)

    def test_rejects_image_bss_over_dtb(self):
        with self.assertRaises(ValueError):
            self.run_image(size=47 * 1024 * 1024)

    def test_rejects_incomplete_image(self):
        with self.assertRaises(ValueError):
            self.run_image(size=32)

    def test_rejects_misaligned_image_base(self):
        with self.assertRaises(ValueError):
            self.run_image(offset=0x80000)

    def test_rejects_wrapped_image_base(self):
        with self.assertRaises(ValueError):
            self.run_image(offset=0x10040200000)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Create a Go SD image from checked boot inputs; never write devices."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

FACTORY = {
    "boot.bin": "465b9f8579c8bf0f7c7d2d0c52852b9801d711d2008d410afcf3b1fdb689e06a",
    "uboot.bin": "f495498134de8598d0ba3d55e387a89fc28ce5beb686574c13d03024a9b30c77",
}
BOOT_START = 2048
BOOT_SECTORS = 262144
ROOT_START = BOOT_START + BOOT_SECTORS


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("boot", "rootfs", "output"):
        p.add_argument("--" + name, type=Path, required=True)
    inputs = p.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--factory", type=Path)
    inputs.add_argument("--firmware", type=Path)
    a = p.parse_args()
    firmware = a.firmware or a.factory
    expected = dict(FACTORY)
    if a.firmware:
        manifest = json.loads((firmware / "manifest.json").read_text())
        boot = json.loads((a.boot / "manifest.json").read_text())
        if manifest["architecture"] != "arm64" or boot["architecture"] != "arm64":
            raise ValueError("ARM64 firmware and boot images must be paired")
        expected = {name: manifest[name]["sha256"] for name in ("boot.bin", "uboot.bin", "atf.img")}
        for name in ("kernel.img", "dtb.img"):
            if hashlib.sha256((a.boot / name).read_bytes()).hexdigest() != boot[name]["sha256"]:
                raise ValueError("boot capsule identity differs: " + name)
    for name, digest in expected.items():
        if hashlib.sha256((firmware / name).read_bytes()).hexdigest() != digest:
            raise ValueError("firmware input identity differs: " + name)
    root_sectors = (a.rootfs.stat().st_size + 511) // 512
    bootfs = a.output.with_suffix(".fat")
    with bootfs.open("wb") as f:
        f.truncate(BOOT_SECTORS * 512)
    subprocess.run(["mkfs.vfat", "--invariant", "-F", "32", "-n", "ONEKVMBOOT", str(bootfs)], check=True)
    for name in (*expected, "kernel.img", "dtb.img"):
        src = (firmware if name in expected else a.boot) / name
        subprocess.run(["mcopy", "-i", str(bootfs), str(src), "::" + name], check=True)
    subprocess.run(["mmd", "-i", str(bootfs), "::onekvm"], check=True)
    mbr = bytearray(512)
    # CHS fields are saturated; both firmware and Linux use LBA.
    for n, (typ, start, size) in enumerate(((0x0c, BOOT_START, BOOT_SECTORS),
                                           (0x83, ROOT_START, root_sectors))):
        struct.pack_into("<B3sB3sII", mbr, 446 + n * 16, 0, b"\xfe\xff\xff", typ,
                         b"\xfe\xff\xff", start, size)
    mbr[510:512] = b"\x55\xaa"
    with a.output.open("wb") as f:
        f.truncate((ROOT_START + root_sectors) * 512)
        f.write(mbr)
    # Sparse output avoids writing the unused ext4 space to the build volume.
    for source, sector in ((bootfs, BOOT_START), (a.rootfs, ROOT_START)):
        subprocess.run(["dd", "if=" + str(source), "of=" + str(a.output), "bs=512",
                        "seek=" + str(sector), "conv=notrunc,sparse", "status=none"], check=True)
    bootfs.unlink()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Wrap OE-built ARM32 Image+initramfs/DTB for the unchanged Go SD loader."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys


def package(sdk, source, output, raw_limit, compressed_limit):
    source = source.resolve()
    raw = source.read_bytes()
    if not raw or len(raw) > raw_limit:
        raise ValueError("image exceeds the Go boot memory window")
    binary = output.with_suffix(".bin")
    binary.write_bytes(raw)
    subprocess.run([str(sdk / "tools/ax_gzip_tool/ax_gzip"), "-9", str(binary)], check=True)
    compressed = binary.with_name(binary.stem + "_axgzip.bin")
    data = compressed.read_bytes()
    compressed.write_bytes(data + bytes(-len(data) % 4))
    if compressed.stat().st_size > compressed_limit:
        raise ValueError("compressed image exceeds the Go boot memory window")
    sys.path.insert(0, str(sdk / "tools/imgsign"))
    spec = importlib.util.spec_from_file_location("ax_image_signer", sdk / "build/tools/imgsign/sec_boot_AX620E_sign.py")
    signer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(signer)
    if not signer.make_image(str(compressed), str(sdk / "tools/imgsign/public.pem"),
                             str(sdk / "tools/imgsign/private.pem"), str(output), 0x54fafe, 2048):
        raise ValueError("Axera image wrapping failed")
    data = output.read_bytes()
    checksum, magic, capability, size = struct.unpack_from("<4I", data)
    word_sum = lambda b: sum(struct.unpack("<%dI" % (len(b) // 4), b)) & 0xffffffff
    if (magic != 0x55543322 or size != len(data) - 1024 or
            checksum != word_sum(data[8:1024]) or
            word_sum(data[1024:]) != struct.unpack_from("<I", data, 20)[0]):
        raise ValueError("invalid Axera package checksums")
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "uncompressed_size": len(raw), "source_sha256": hashlib.sha256(raw).hexdigest()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("sdk", "kernel", "dtb", "out"):
        p.add_argument("--" + name, type=Path, required=True)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    kernel = a.kernel.read_bytes()
    if len(kernel) >= 40 and struct.unpack_from("<I", kernel, 36)[0] == 0x016f2818:
        raise ValueError("Go sd_boot needs raw ARM Image, not zImage")
    if a.dtb.read_bytes()[:4] != bytes.fromhex("d00dfeed"):
        raise ValueError("invalid DTB")
    # Factory ARM32 loader: Image 0x40008000, DTB 0x42008000.
    # Compressed DTB/kernel sit at UBOOT_IMG_HEADER_BASE + 4/5 MiB,
    # with SD staging at +45 MiB. Keep all four regions disjoint.
    mib = 1024 * 1024
    manifest = {name: package(a.sdk.resolve(), src, a.out / name, raw_limit, compressed_limit)
                for name, src, raw_limit, compressed_limit in (
                    ("kernel.img", a.kernel, 32 * mib, 40 * mib),
                    ("dtb.img", a.dtb, mib, mib))}
    (a.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()

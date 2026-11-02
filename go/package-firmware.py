#!/usr/bin/env python3
"""Compose Go AArch64 ROM/SPL/BL31/U-Boot SD files; never write devices."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("sdk", "firmware", "out"):
        p.add_argument("--" + name, type=Path, required=True)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(a.sdk / "tools/imgsign"))
    boot = module("go_boot", Path(__file__).with_name("package-boot.py"))
    signer = module("go_spl_signer", a.sdk / "build/tools/imgsign/spl_AX620E_sign.py")
    spl = a.firmware / "spl.bin"
    payload = spl.read_bytes()
    if not 0 < len(payload) <= 50 * 1024 or len(payload) % 4:
        raise ValueError("SPL must fit before the ROM package EIP firmware")
    (a.out / "boot.bin").unlink(missing_ok=True)
    signer.do_spl(str(spl), str(a.sdk / "tools/imgsign/public.pem"),
                  str(a.sdk / "tools/imgsign/private.pem"), str(a.out / "boot.bin"),
                  str(a.sdk / "build/tools/imgsign/eip_ax620e.bin"), None, 0x54fafe)
    image = (a.out / "boot.bin").read_bytes()
    if len(image) != 256 * 1024 or image[1024:1024 + len(payload)] != payload:
        raise ValueError("invalid SPL ROM package")
    for offset in (0, 128 * 1024):
        if (image[offset + 4:offset + 8] != bytes.fromhex("22335455") or
                image[offset + 1024:offset + 1024 + len(payload)] != payload):
            raise ValueError("invalid primary/secondary SPL package")
    manifest = {
        "architecture": "arm64", "boot_medium": "sd", "optee": False,
        "bl33_el": 1, "dram_bytes": 256 * 1024 * 1024,
        "persistent_environment": False, "hardware_qualified": False,
        "boot.bin": {"size": len(image), "sha256": hashlib.sha256(image).hexdigest(),
                     "source_sha256": hashlib.sha256(payload).hexdigest()},
    }
    for name, source, limit in (("uboot.bin", "u-boot.bin", 2 * 1024 * 1024),
                                ("atf.img", "bl31.bin", 256 * 1024)):
        manifest[name] = boot.package(a.sdk, a.firmware / source, a.out / name,
                                     limit, limit + 1024, compress=False)
    (a.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()

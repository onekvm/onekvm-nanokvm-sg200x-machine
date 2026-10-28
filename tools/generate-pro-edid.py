#!/usr/bin/env python3
"""Generate the Pro's 256-byte EDIDs from CTA/DMT timings, without dependencies.

Run from any directory. Pro presets live below share/edid/pro so the NanoKVM
recipe's share/edid/*.edid.bin glob cannot install unsupported 4K modes.
"""

from pathlib import Path
from edid_modes import generate_common


# name, pixel clock (kHz), active/blanking, sync front porch/width, polarity, VIC.
# 1440p uses DMT 0x58 (CVT reduced blanking), nominally 59.95 Hz.
MODES = [
    ("720p60", 74250, 1280, 370, 720, 30, 110, 40, 5, 5, 0x1E, 4),
    ("1080p60", 148500, 1920, 280, 1080, 45, 88, 44, 4, 5, 0x1E, 16),
    ("1440p60", 241500, 2560, 160, 1440, 41, 48, 32, 3, 5, 0x1A, 0),
    ("2160p30", 297000, 3840, 560, 2160, 90, 176, 88, 8, 10, 0x1E, 95),
]
MODE_1080P120 = (
    "1080p120", 297000, 1920, 280, 1080, 45, 88, 44, 4, 5, 0x1E, 63,
)

MODE_1440P30 = (
    "1440p30", 120360, 2560, 160, 1440, 35, 48, 32, 3, 5, 0x1A, 0,
)


def timing(mode):
    _, clock, w, hb, h, vb, hf, hs, vf, vs, flags, _ = mode
    d = bytearray(18)
    d[:2] = (clock // 10).to_bytes(2, "little")
    d[2:8] = bytes((w & 255, hb & 255, (w >> 8) << 4 | hb >> 8,
                    h & 255, vb & 255, (h >> 8) << 4 | vb >> 8))
    d[8:12] = bytes((hf & 255, hs & 255, (vf & 15) << 4 | vs & 15,
                     (hf >> 8) << 6 | (hs >> 8) << 4 | (vf >> 4) << 2 | vs >> 4))
    # Match the base EDID's 64 x 36 cm image size in every detailed timing.
    d[12:15] = bytes((640 & 255, 360 & 255, (640 >> 8) << 4 | 360 >> 8))
    d[17] = flags
    return d


def descriptor(tag, payload):
    return b"\0\0\0" + bytes((tag, 0)) + payload


def data_block(tag, payload):
    return bytes(((tag << 5) | len(payload),)) + bytes(payload)


def checksum(block):
    block[127] = -sum(block[:127]) & 255
    return block


def generate(index, modes=MODES, display_name=None):
    assert 0 <= index < len(modes)
    mode = modes[index]
    name = mode[0]
    clock = max(m[1] for m in modes[:index + 1])
    base = bytearray(128)
    base[:8] = bytes.fromhex("00ffffffffffff00")
    base[8:10] = bytes.fromhex("3d76")  # OKV (OneKVM).
    base[10:12] = (0x630C).to_bytes(2, "little")
    base[16:25] = bytes((1, 36, 1, 3, 0x80, 64, 36, 120, 0x0E))
    base[25:35] = bytes.fromhex("ee91a3544c99260f5054")  # sRGB.
    base[35:38] = bytes((0xA1, 0x08, 0))  # BIOS-safe established timings.
    base[38:54] = b"\x01\x01" * 8
    base[54:72] = timing(mode)
    label = (display_name or "OneKVM " + (name.replace("2160p", "4K")
                                         if index >= 3 else name.removesuffix("60"))).encode()
    assert len(label) <= 12
    base[72:90] = descriptor(0xFC, (label + b"\n").ljust(13, b" "))
    max_h = max(49, max((m[1] + m[2] + m[3] - 1) // (m[2] + m[3])
                        for m in modes[:index + 1]))
    max_v = max(75, max((m[1] * 1000 + (m[2] + m[3]) * (m[4] + m[5]) - 1)
                        // ((m[2] + m[3]) * (m[4] + m[5]))
                        for m in modes[:index + 1]))
    limits = bytes((24, max_v, 15, max_h, (clock + 9999) // 10000, 0, 10))
    base[90:108] = descriptor(0xFD, limits + b" " * 6)
    base[108:126] = descriptor(0x10, bytes(13))
    base[126] = 1

    # Each preset advertises its preferred timing and lower-bandwidth modes.
    # The first base DTD is preferred. 1440p has no CTA VIC.
    vics = [m[-1] for m in reversed(modes[:index + 1]) if m[-1]]
    vics.append(1)  # 640x480p60.
    blocks = data_block(2, vics)
    blocks += data_block(1, (0x09, 0x04, 0x07))  # Stereo LPCM, 48 kHz, 16/20/24 bit.
    blocks += data_block(4, (1, 0, 0))  # Front left/right.
    hdmi = [3, 12, 0, 0x10, 0, 0, min(60, (clock + 4999) // 5000)]
    if index >= 3:
        hdmi += [0x20, 0, 0x20, 1]  # HDMI 1.4 VIC 1: 3840x2160p30.
    blocks += data_block(3, hdmi)
    # VCDB: selectable RGB/YCbCr quantization; CE and IT modes underscanned.
    blocks += data_block(7, (0, 0xCA))
    cta = bytearray(128)
    # The preferred timing is the base-block DTD.  The optional CTA DTD for
    # 2160p30 is an additional mode and must not be advertised as native.
    cta[:4] = bytes((2, 3, 4 + len(blocks), 0xf0))
    cta[4:4 + len(blocks)] = blocks
    if index >= 3:
        offset = 4 + len(blocks)
        cta[offset:offset + 18] = timing(modes[2])
    return bytes(checksum(base) + checksum(cta))


if __name__ == "__main__":
    destination = Path(__file__).resolve().parents[1] / "share/edid/pro"
    destination.mkdir(parents=True, exist_ok=True)
    for index, mode in enumerate(MODES):
        (destination / (mode[0] + ".edid.bin")).write_bytes(generate(index))
    (destination / "1080p120.edid.bin").write_bytes(generate(
        2, modes=(*MODES[:2], MODE_1080P120), display_name="OneKVM 120Hz"))
    (destination / "1440p30.edid.bin").write_bytes(generate(
        2, modes=(*MODES[:2], MODE_1440P30), display_name="OneKVM QHD30"))
    generate_common(destination, pro=True)
    generate_common(destination.parent, pro=False)

"""Common 60 Hz EDID modes, grouped by nominal display aspect ratio.

17:9 and 21:9 include the usual DCI and ultrawide marketing ratios. Timings
use DMT/CTA where available, otherwise reduced blanking with a 25 MHz floor.
Adding a preset does not establish end-to-end capture support for every source.
"""
import math


RESOLUTIONS = {
    '1:1': [(480, 480), (600, 600), (720, 720), (800, 800), (900, 900), (1024, 1024), (1080, 1080)],
    '5:4': [(600, 480), (800, 640), (960, 768), (1280, 1024)],
    '4:3': [(640, 480), (800, 600), (960, 720), (1024, 768), (1152, 864), (1280, 960), (1440, 1080)],
    '3:2': [(720, 480), (960, 640), (1080, 720), (1200, 800), (1440, 960), (1536, 1024), (1620, 1080)],
    '16:10': [(768, 480), (960, 600), (1152, 720), (1280, 800), (1440, 900), (1536, 960), (1680, 1050), (1728, 1080)],
    '16:9': [(854, 480), (960, 540), (1024, 576), (1280, 720), (1600, 900), (1920, 1080)],
    '17:9': [(1024, 540), (1360, 720), (1536, 810), (2048, 1080)],
    '21:9': [(1120, 480), (1280, 540), (1680, 720), (1920, 810), (2100, 900), (2560, 1080)],
    '32:9': [(1728, 486), (1920, 540), (2560, 720), (3200, 900), (3840, 1080)],
    '9:16': [(480, 854), (540, 960), (576, 1024), (720, 1280), (900, 1600), (1080, 1920)],
}

# clock kHz, H blank/front/sync, V blank/front/sync, DTD flags.
STANDARD = {
    (640, 480): (25175, 160, 16, 96, 45, 10, 2, 0x18),
    (800, 600): (40000, 256, 40, 128, 28, 1, 4, 0x1E),
    (1024, 768): (65000, 320, 24, 136, 38, 3, 6, 0x18),
    (1280, 1024): (108000, 408, 48, 112, 42, 1, 3, 0x1E),
    (1280, 720): (74250, 370, 110, 40, 30, 5, 5, 0x1E),
    (1920, 1080): (148500, 280, 88, 44, 45, 4, 5, 0x1E),
}


def mode_timing(width, height, aspect):
    if (width, height) in STANDARD:
        return STANDARD[width, height]
    # A 460 us minimum vertical blank; 48/32/80 horizontal porches/sync.
    period = (1_000_000 / 60 - 460) / height
    vs = {'4:3': 4, '16:9': 5, '16:10': 6, '5:4': 7}.get(aspect, 10)
    vb = max(math.ceil(460 / period), 3 + vs + 6)
    hb = 160
    while True:
        clock = math.floor((width + hb) * (height + vb) * 60 / 250_000) * 250
        if clock >= 25000:
            return clock, hb, 48, 32, vb, 3, vs, 0x1A
        hb += 8


def generate_mode(width, height, aspect, pro):
    clock, hb, hf, hs, vb, vf, vs, flags = mode_timing(width, height, aspect)
    dtd = bytearray(18)
    dtd[:2] = (round(clock / 10)).to_bytes(2, 'little')
    dtd[2:8] = bytes((width & 255, hb & 255, (width >> 8) << 4 | hb >> 8,
                     height & 255, vb & 255, (height >> 8) << 4 | vb >> 8))
    dtd[8:12] = bytes((hf & 255, hs & 255, (vf & 15) << 4 | vs & 15,
                      (hf >> 8) << 6 | (hs >> 8) << 4 | (vf >> 4) << 2 | vs >> 4))
    wc = 36 if width < height else 64
    hc = round(wc * height / width)
    wm, hm = wc * 10, hc * 10
    dtd[12:15] = bytes((wm & 255, hm & 255, (wm >> 8) << 4 | hm >> 8))
    dtd[17] = flags
    base = bytearray(128)
    base[:8] = bytes.fromhex('00ffffffffffff00')
    base[8:10] = bytes.fromhex('3d76')
    base[10:12] = (0x630C if pro else 1).to_bytes(2, 'little')
    base[16:25] = bytes((1, 36, 1, 3, 0x80, wc, hc, 120, 0x0E))
    base[25:35] = bytes.fromhex('ee91a3544c99260f5054')
    base[35] = 0x20  # BIOS-safe 640x480p60 only.
    base[38:54] = b'\x01\x01' * 8
    base[54:72] = dtd
    name = f'OKV{width}x{height}'.encode()
    assert len(name) <= 12
    base[72:90] = b'\0\0\0\xfc\0' + (name + b'\n').ljust(13, b' ')
    max_h = max(32, math.ceil(clock / (width + hb)))
    max_clock = max(3, math.ceil(clock / 10000))
    base[90:108] = b'\0\0\0\xfd\0' + bytes((50, 75, 15, max_h, max_clock, 0, 10)) + b' ' * 6
    base[108:126] = b'\0\0\0\x10\0' + bytes(13)
    base[126] = 1
    # HDMI, VGA fallback and stereo LPCM; preferred mode stays in the base DTD.
    blocks = bytes((0x41, 1, 0x23, 0x09, 0x04, 0x07, 0x83, 1, 0, 0,
                    0x67, 3, 12, 0, 0x10, 0, 0, max(5, math.ceil(clock / 5000)),
                    0xE2, 0, 0xCA))
    cta = bytearray(128)
    cta[:4] = bytes((2, 3, 4 + len(blocks), 0xf0))
    cta[4:4 + len(blocks)] = blocks
    for block in (base, cta):
        block[127] = -sum(block[:127]) & 255
    return bytes(base + cta)


def generate_common(destination, pro):
    skipped = {(1920, 1080)}
    if pro:
        skipped.add((1280, 720))  # Existing 720p60 preset retains its ID.
    for aspect, sizes in RESOLUTIONS.items():
        for width, height in sizes:
            clock = mode_timing(width, height, aspect)[0]
            if (width, height) in skipped:
                continue
            if width > (3840 if pro else 2880) or height > (2160 if pro else 1620):
                continue
            if clock > (297000 if pro else 165000):
                continue
            (destination / f'{width}x{height}p60.edid.bin').write_bytes(
                generate_mode(width, height, aspect, pro))

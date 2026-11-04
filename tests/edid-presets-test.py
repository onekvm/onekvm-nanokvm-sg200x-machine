"""Validate shipped bytes independently of EDID encoding helpers."""
from pathlib import Path
import math
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
from edid_modes import RESOLUTIONS

# Pixel clocks from CTA-861, independent of the preset generators. Include
# fallback VICs: even a 25 MHz preferred timing advertises 25.175 MHz VGA.
CTA_CLOCK_KHZ = {1: 25175, 4: 74250, 16: 148500, 63: 297000, 95: 297000}


def validate_tmds_clock(data, path):
    clocks = [int.from_bytes(data[i:i + 2], 'little') * 10
              for i in range(54, 126, 18) if data[i] or data[i + 1]]
    end = 128 + data[130]
    limits = []
    offset = 132
    while offset < end:
        header = data[offset]
        length = header & 31
        assert offset + 1 + length <= end, path
        payload = data[offset + 1:offset + 1 + length]
        if header >> 5 == 2:
            clocks.extend(CTA_CLOCK_KHZ[vic & 127] for vic in payload)
        elif header >> 5 == 3 and payload[:3] == bytes.fromhex('030c00'):
            assert length >= 7, path
            if payload[6]:  # Zero means the maximum was not declared.
                limits.append(payload[6] * 5000)
        offset += 1 + length
    for offset in range(end, 237, 18):
        if data[offset] or data[offset + 1]:
            clocks.append(int.from_bytes(data[offset:offset + 2], 'little') * 10)
    for limit in limits:
        assert limit >= max(clocks), (path, 'TMDS ceiling kHz', limit, 'required kHz', max(clocks))


for pro in [False, True]:
    directory = root / 'share/edid' / ('pro' if pro else '')
    files = list(directory.glob('*.edid.bin'))
    assert (directory / '1920x1080p90.edid.bin').is_file()
    assert not (directory / '1080p60-4k30.edid.bin').exists()
    for path in files:
        data = path.read_bytes()
        assert len(data) == 256 and data[:8] == bytes.fromhex('00ffffffffffff00'), path
        assert data[126] == 1 and data[128:130] == b'\x02\x03', path
        # CTA byte 3 advertises underscan/basic audio/YCbCr support.  A
        # preferred base-block DTD must not be paired with a bogus native CTA
        # DTD count.
        if path.name != 'factory.edid.bin':
            assert data[131] & 0x80 and (data[131] & 0x0f) == 0, path
        assert sum(data[:128]) % 256 == sum(data[128:]) % 256 == 0, path
        validate_tmds_clock(data, path)
        if path.name in ('factory.edid.bin', '1080p60.edid.bin'):
            d = data[54:72]
            w, h = d[2] | (d[4] >> 4) << 8, d[5] | (d[7] >> 4) << 8
            hb, vb = d[3] | (d[4] & 15) << 8, d[6] | (d[7] & 15) << 8
            hz = int.from_bytes(d[:2], 'little') * 10000 / ((w + hb) * (h + vb))
            assert (w, h) == (1920, 1080) and abs(hz - 60) < 0.01, path
        if 'x' not in path.stem:
            continue
        width, rest = path.name.split('x')
        height = rest.split('p')[0]
        width, height = int(width), int(height)
        d = data[54:72]
        w = d[2] | (d[4] >> 4) << 8
        h = d[5] | (d[7] >> 4) << 8
        hb = d[3] | (d[4] & 15) << 8
        vb = d[6] | (d[7] & 15) << 8
        hz = int.from_bytes(d[:2], 'little') * 10000 / ((w + hb) * (h + vb))
        assert (w, h) == (width, height), path
        refresh = int(rest.split('p')[1].split('.')[0])
        assert refresh in (60, 90), path
        assert w % 2 == h % 2 == 0 and abs(hz - refresh) < 1, (path, hz)
        assert w <= (3840 if pro else 2880) and h <= (2160 if pro else 1620), path
        if refresh == 90:
            assert (w, h) == (1920, 1080) and abs(hz - 90) < 0.001, (path, hz)
            assert int.from_bytes(d[:2], 'little') * 10 == 222750, path
            assert data[96] >= 90 and data[99] * 10000 >= 222750, path
            assert data[132:136] == bytes((0x43, 16, 4, 1)), path
        else:
            assert int.from_bytes(d[:2], 'little') * 10 <= (297000 if pro else 165000), path
        assert (d[8] | ((d[11] >> 6) << 8)) + (d[9] | (((d[11] >> 4) & 3) << 8)) <= hb, path
        assert (d[10] >> 4 | (((d[11] >> 2) & 3) << 4)) + (d[10] & 15 | ((d[11] & 3) << 4)) <= vb, path
    if pro:
        for name, size, refresh in [('2160p30', (3840, 2160), 30),
                                    ('1440p60', (2560, 1440), 60),
                                    ('1440p30', (2560, 1440), 30)]:
            data = (directory / (name + '.edid.bin')).read_bytes()
            d = data[54:72]
            w, h = d[2] | (d[4] >> 4) << 8, d[5] | (d[7] >> 4) << 8
            hb, vb = d[3] | (d[4] & 15) << 8, d[6] | (d[7] & 15) << 8
            hz = int.from_bytes(d[:2], 'little') * 10000 / ((w + hb) * (h + vb))
            assert (w, h) == size and abs(hz - refresh) < 0.1, (name, w, h, hz)
            assert int.from_bytes(d[:2], 'little') * 10 <= 297000, name
    for aspect, sizes in RESOLUTIONS.items():
        assert any((directory / f'{w}x{h}p60.edid.bin').exists() for w, h in sizes), aspect
    print(('Pro' if pro else 'SG200x'), len(files), 'EDIDs: checksums, timing, TMDS ceilings, default 1080p60 and optional 1080p90 passed')

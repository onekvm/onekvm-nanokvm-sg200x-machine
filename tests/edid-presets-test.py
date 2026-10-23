"""Validate shipped bytes independently of EDID encoding helpers."""
from pathlib import Path
import math
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
from edid_modes import RESOLUTIONS

for pro in [False, True]:
    directory = root / 'share/edid' / ('pro' if pro else '')
    files = list(directory.glob('*.edid.bin'))
    assert not (directory / '1080p60-4k30.edid.bin').exists()
    for path in files:
        data = path.read_bytes()
        assert len(data) == 256 and data[:8] == bytes.fromhex('00ffffffffffff00'), path
        assert data[126] == 1 and data[128:130] == b'\x02\x03', path
        assert sum(data[:128]) % 256 == sum(data[128:]) % 256 == 0, path
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
        assert w % 2 == h % 2 == 0 and abs(hz - 60) < 1, (path, hz)
        assert w <= (3840 if pro else 2880) and h <= (2160 if pro else 1620), path
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
    print(('Pro' if pro else 'SG200x'), len(files), 'EDIDs: all ten ratios, checksums, timing and bounds passed')

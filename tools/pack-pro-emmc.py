#!/usr/bin/env python3
"""Assemble a NanoKVM Pro raw eMMC image from the vendor partition XML.

The published image has no MBR and no GPT. Partition sizes in the XML use
unit 2, which is 1 KiB, and the ranges are placed back to back from byte 0.
A size of 0xffffffff is only valid on the last partition; the image ends at
the expanded payload. The 1.0.15 rootfs expands to 4608 MiB of ext4.

Android sparse payloads (magic 0xED26FF3A) are expanded. Zero fill stays a
hole. The result contains the vendor boot chain when those files are packed,
so it must not be written with dd from LBA 0 onto a device whose bootloader
should be kept.
"""
import argparse
import os
from pathlib import Path
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile

KIB = 1024
SPARSE_MAGIC = 0xED26FF3A
SPARSE_RAW = 0xCAC1
SPARSE_FILL = 0xCAC2
SPARSE_DONT_CARE = 0xCAC3
SPARSE_CRC32 = 0xCAC4
COPY_CHUNK = 1024 * 1024


class Partition:
    def __init__(self, name, offset, size):
        self.name = name
        self.offset = offset
        self.size = size  # None means the image ends at this payload.


def parse_partitions(xml_text):
    root = ET.fromstring(xml_text)
    node = root.find('.//Partitions')
    if node is None:
        raise ValueError('partition XML has no Partitions element')
    if node.get('unit') != '2':
        raise ValueError('only unit=2 (1 KiB) partition XML is supported')
    elements = list(node.findall('Partition'))
    if not elements:
        raise ValueError('partition XML has no partitions')
    partitions = []
    offset = 0
    for index, part in enumerate(elements):
        raw = part.get('size')
        name = part.get('id')
        if not name or raw is None:
            raise ValueError('partition %s is missing id or size' % index)
        if raw.startswith('0x'):
            if int(raw, 16) != 0xFFFFFFFF or index != len(elements) - 1:
                raise ValueError('unbounded size is only valid on the last partition')
            partitions.append(Partition(name, offset, None))
            continue
        size = int(raw) * KIB
        if size <= 0:
            raise ValueError('partition %s has an empty size' % name)
        partitions.append(Partition(name, offset, size))
        offset += size
    return partitions


def payload_map(xml_text):
    """Return partition id -> filename for image nodes that name a file."""
    root = ET.fromstring(xml_text)
    found = {}
    for img in root.findall('.//Img'):
        block = img.find('Block')
        file_node = img.find('File')
        if block is None or file_node is None or not (file_node.text or '').strip():
            continue
        part_id = block.get('id')
        if part_id:
            found[part_id] = file_node.text.strip()
    return found


def _sparse_header(blob):
    if len(blob) < 28:
        return None
    magic, major, _minor, file_hdr, chunk_hdr, blk, blocks, _chunks, _csum = struct.unpack_from(
        '<IHHHHIIII', blob)
    if magic != SPARSE_MAGIC:
        return None
    if major != 1 or file_hdr < 28 or chunk_hdr < 12 or blk == 0:
        raise ValueError('unsupported Android sparse header')
    return file_hdr, chunk_hdr, blk, blocks


def sparse_expanded_size(header):
    parsed = _sparse_header(header)
    if parsed is None:
        return None
    _file_hdr, _chunk_hdr, blk, blocks = parsed
    return blocks * blk


def expand_sparse(source, dest, dest_offset, progress=None):
    """Expand an Android sparse image at dest_offset. Return the expanded size."""
    header = source.read(28)
    parsed = _sparse_header(header)
    if parsed is None:
        raise ValueError('payload is not an Android sparse image')
    file_hdr, chunk_hdr, blk, blocks = parsed
    if file_hdr > 28:
        source.read(file_hdr - 28)
    if blocks == 0:
        return 0
    written_blocks = 0
    output_end = dest_offset
    chunk_index = 0
    while written_blocks < blocks:
        chunk = source.read(chunk_hdr)
        if not chunk:
            break
        if len(chunk) != chunk_hdr:
            raise ValueError('truncated sparse chunk header')
        kind, _reserved, count, total = struct.unpack_from('<HHII', chunk)
        payload = total - chunk_hdr
        block_bytes = count * blk
        if kind == SPARSE_RAW:
            if payload != block_bytes:
                raise ValueError('sparse raw chunk %s has %s bytes for %s blocks' %
                                 (chunk_index, payload, count))
            remaining = payload
            while remaining:
                buf = source.read(min(COPY_CHUNK, remaining))
                if not buf:
                    raise ValueError('truncated sparse raw chunk')
                os.write(dest, buf)
                remaining -= len(buf)
            output_end += block_bytes
        elif kind == SPARSE_FILL:
            if payload < 4:
                raise ValueError('sparse fill chunk is too small')
            pattern = source.read(4)
            extra = payload - 4
            if extra:
                source.seek(extra, os.SEEK_CUR)
            if pattern != b'\x00\x00\x00\x00':
                tile = pattern * (COPY_CHUNK // 4)
                remaining = block_bytes
                while remaining:
                    take = min(remaining, len(tile))
                    os.write(dest, tile[:take])
                    remaining -= take
            output_end += block_bytes
            if pattern == b'\x00\x00\x00\x00':
                os.lseek(dest, output_end, os.SEEK_SET)
        elif kind == SPARSE_DONT_CARE:
            if payload:
                source.seek(payload, os.SEEK_CUR)
            output_end += block_bytes
            os.lseek(dest, output_end, os.SEEK_SET)
        elif kind == SPARSE_CRC32:
            if payload:
                source.seek(payload, os.SEEK_CUR)
        else:
            raise ValueError('unknown sparse chunk type %#x' % kind)
        if kind != SPARSE_CRC32:
            written_blocks += count
        chunk_index += 1
        if progress and chunk_index % 500 == 0:
            progress(output_end)
        if written_blocks > blocks:
            raise ValueError('sparse image has more blocks than its header')
        if written_blocks == blocks:
            break
    if written_blocks != blocks:
        raise ValueError('sparse image expanded %s blocks, header says %s' %
                         (written_blocks, blocks))
    return blocks * blk


def _write_raw(source, dest, head):
    if head:
        os.write(dest, head)
    total = len(head)
    while True:
        buf = source.read(COPY_CHUNK)
        if not buf:
            break
        os.write(dest, buf)
        total += len(buf)
    return total


def pack_image(xml_text, opener, output, progress=None):
    """Write a raw image. opener(filename) returns a binary file object."""
    partitions = parse_partitions(xml_text)
    names = payload_map(xml_text)
    known = {part.name for part in partitions}
    missing = sorted(name for name in names if name not in known)
    if missing:
        raise ValueError('payloads do not match a partition: %s' % ', '.join(missing))
    fd = os.open(output, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o644)
    image_end = partitions[-1].offset
    placed = []
    try:
        for part in partitions:
            filename = names.get(part.name)
            if not filename:
                if part.size is None:
                    raise ValueError('the last partition %s has no payload' % part.name)
                continue
            source = opener(filename)
            if source is None:
                raise ValueError('missing payload %s for %s' % (filename, part.name))
            with source:
                head = source.read(28)
                expanded = sparse_expanded_size(head)
                os.lseek(fd, part.offset, os.SEEK_SET)
                if expanded is None:
                    size = _write_raw(source, fd, head)
                else:
                    source.seek(0)
                    size = expand_sparse(source, fd, part.offset, progress)
            if part.size is not None and size > part.size:
                raise ValueError('%s payload is %s bytes and partition is %s' %
                                 (part.name, size, part.size))
            end = part.offset + size
            image_end = max(image_end, end)
            placed.append((part, filename, size))
            if progress:
                progress(end)
        os.ftruncate(fd, image_end)
        os.fsync(fd)
    finally:
        os.close(fd)
    return partitions, placed, image_end


def layout_text(partitions, placed, image_end):
    sizes = {part.name: size for part, _filename, size in placed}
    files = {part.name: filename for part, filename, _size in placed}
    lines = []
    for part in partitions:
        size = part.size if part.size is not None else sizes.get(part.name, 0)
        lines.append('%s offset=%d size=%d payload=%s payload_bytes=%s' % (
            part.name, part.offset, size, files.get(part.name, '-'),
            sizes.get(part.name, 0)))
    lines.append('image_bytes=%d' % image_end)
    return '\n'.join(lines) + '\n'


def compare_images(left, right_open, chunk=4 * COPY_CHUNK):
    """Return the first mismatch offset, or None when both streams are equal."""
    with open(left, 'rb') as source, right_open() as other:
        offset = 0
        while True:
            a = source.read(chunk)
            b = other.read(len(a) if a else chunk)
            if a != b:
                limit = min(len(a), len(b))
                for index in range(limit):
                    if a[index] != b[index]:
                        return offset + index
                return offset + limit
            if not a:
                return None
            offset += len(a)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--axp', required=True, help='vendor .axp zip with the partition XML and payloads')
    parser.add_argument('--output', required=True)
    parser.add_argument('--compare', help='xz-compressed raw image to compare after packing')
    args = parser.parse_args(argv)
    archive = zipfile.ZipFile(args.axp)
    xml_name = next(name for name in archive.namelist() if name.endswith('.xml'))
    xml_text = archive.read(xml_name).decode('utf-8')

    def opener(name):
        return archive.open(name)

    def progress(done):
        print('packed %d MiB' % (done // (1024 * 1024)), file=sys.stderr, flush=True)

    partitions, placed, image_end = pack_image(xml_text, opener, args.output, progress)
    text = layout_text(partitions, placed, image_end)
    Path(args.output + '.layout').write_text(text)
    sys.stdout.write(text)
    if args.compare:
        import lzma
        mismatch = compare_images(args.output, lambda: lzma.open(args.compare, 'rb'))
        if mismatch is not None:
            raise SystemExit('image differs at byte %d' % mismatch)
        print('compare=match')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))

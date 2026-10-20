#!/usr/bin/env python3
"""Common compact SD carrier / vendor-offset eMMC image assembly.

The firstboot hook turns either SD carrier into the final four-partition layout.
eMMC uses Linux cmdline partitions, so its bootloader prefix has no MBR.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct

ROOTFS_SLOT_SECTORS = 128 * 1024 * 1024 // 512

# The image trailer is deliberately sector-sized.  It is outside every
# partition/payload and is therefore ignored by the boot chain, while a host
# side flasher can inspect it before touching a target device.
TRAILER_SIZE = 512
TRAILER_HASH_SIZE = 32
TRAILER_METADATA_SIZE = TRAILER_SIZE - TRAILER_HASH_SIZE
TRAILER_MAGIC = b'ONEKVM-IMG-META\0'
TRAILER_VERSION = 1

# Keep the IDs stable: the recovery image installer uses these values without
# Python.  The profile also captures the board family, not only SD vs eMMC.
PROFILE_IDS = {'nanokvm': 1, 'pro-sd': 2, 'pro-emmc': 3}
MEDIA_IDS = {'nanokvm': 1, 'pro-sd': 1, 'pro-emmc': 2}
MACHINE_NAMES = {'nanokvm': 'onekvm-nanokvm', 'pro-sd': 'onekvm-nanokvm-pro',
                 'pro-emmc': 'onekvm-nanokvm-pro'}

def layout(disk_id, boot_at, boot_size, root_at, root_b_gap=0):
    return (disk_id, boot_at, boot_size, root_at, ROOTFS_SLOT_SECTORS,
            root_at + ROOTFS_SLOT_SECTORS + root_b_gap,
            root_at + 2 * ROOTFS_SLOT_SECTORS + root_b_gap)

LAYOUTS = {
    # Keep the 1 MiB gap before B for the Cube's 2 MiB alignment.
    'nanokvm': layout(0x4f4b564d, 1, 262144, 264192, root_b_gap=2048),
    'pro-sd': layout(0x4f4b5650, 2048, 262144, 264192),
    'pro-emmc': layout(None, 306688, 262144, 568832),
}

def digest(path, offset=0, size=None):
    result = hashlib.sha256()
    with open(path, 'rb') as source:
        source.seek(offset)
        while size is None or size:
            block = source.read(1048576 if size is None else min(size, 1048576))
            if not block:
                if size:
                    raise ValueError('short payload read')
                break
            result.update(block)
            if size is not None:
                size -= len(block)
    return result.hexdigest()


def _fixed_ascii(value, size, field):
    encoded = value.encode('ascii')
    if not encoded or len(encoded) >= size:
        raise ValueError('%s is empty or too long' % field)
    return encoded + bytes(size - len(encoded))


def make_trailer(profile, payload_size, payload_sha256, build_id):
    """Return metadata followed by its full-image digest placeholder.

    The final digest is calculated by :func:`append_trailer` after the
    metadata has been written.  Keeping the payload digest in metadata lets a
    writer verify exactly the bytes it will copy, without including this
    trailer in the target storage layout.
    """
    if profile not in PROFILE_IDS:
        raise ValueError('unknown image profile: %s' % profile)
    if payload_size <= 0:
        raise ValueError('image payload is empty')
    build = _fixed_ascii(build_id, 96, 'build_id')
    machine = _fixed_ascii(MACHINE_NAMES[profile], 64, 'machine')
    try:
        payload_digest = bytes.fromhex(payload_sha256)
    except ValueError as error:
        raise ValueError('invalid payload SHA-256') from error
    if len(payload_digest) != TRAILER_HASH_SIZE:
        raise ValueError('invalid payload SHA-256 length')

    metadata = bytearray(TRAILER_METADATA_SIZE)
    metadata[:16] = TRAILER_MAGIC
    struct.pack_into('<HHBBHQHH', metadata, 16,
                     TRAILER_VERSION, TRAILER_METADATA_SIZE,
                     PROFILE_IDS[profile], MEDIA_IDS[profile], 0,
                     payload_size, 96, 0)
    metadata[36:68] = payload_digest
    metadata[68:164] = build
    struct.pack_into('<HH', metadata, 164, len(MACHINE_NAMES[profile]), 0)
    metadata[168:232] = machine
    return bytes(metadata)


def append_trailer(output, profile, payload_size, build_id):
    payload_sha256 = digest(output, size=payload_size)
    metadata = make_trailer(profile, payload_size, payload_sha256, build_id)
    with open(output, 'ab') as dest:
        dest.write(metadata)
        dest.flush()
        os.fsync(dest.fileno())
    full_sha256 = digest(output)
    with open(output, 'ab') as dest:
        dest.write(bytes.fromhex(full_sha256))
        dest.flush()
        os.fsync(dest.fileno())
    assert Path(output).stat().st_size == payload_size + TRAILER_SIZE
    return payload_sha256, full_sha256

def build(profile, boot, rootfs, output, build_id, raw=()):
    disk_id, boot_at, target_boot_size, target_root_at, slot_size, root_b, data_at = LAYOUTS[profile]
    boot_bytes = Path(boot).stat().st_size
    boot_size = boot_bytes // 512
    if profile == 'pro-emmc':
        # Cmdline partitions and vendor raw offsets are fixed on eMMC.
        if boot_size != target_boot_size or boot_bytes % 512:
            raise ValueError('eMMC boot image must match the cmdline partition')
        root_at = target_root_at
    else:
        if boot_bytes % 1048576 or not 16 * 1048576 <= boot_bytes <= target_boot_size * 512:
            raise ValueError('SD carrier boot must be MiB-aligned and between 16 and 128 MiB')
        root_at = ((boot_at + boot_size + 2047) // 2048) * 2048
    root_bytes = Path(rootfs).stat().st_size
    if not 0 < root_bytes <= slot_size * 512:
        raise ValueError('SquashFS is %d bytes; rootfs slot capacity is %d bytes (128 MiB)' %
                         (root_bytes, slot_size * 512))
    with open(rootfs, 'rb') as source:
        header = source.read(48)
    assert header[:4] == b'hsqs' and 4096 <= struct.unpack_from('<Q', header, 40)[0] <= root_bytes
    root_sectors = (root_bytes + 511) // 512
    payloads = [(Path(boot), boot_at * 512), (Path(rootfs), root_at * 512)]
    if profile == 'pro-emmc':
        payloads.extend(raw)
        image_size = (root_at + root_sectors) * 512
        table = 'cmdline'
    else:
        assert not raw
        image_size = (root_at + root_sectors) * 512
        table = 'mbr'
    ranges = sorted((at, at + path.stat().st_size) for path, at in payloads)
    assert all(left[1] <= right[0] for left, right in zip(ranges, ranges[1:])), 'overlapping payloads'
    assert ranges[-1][1] <= image_size
    with open(output, 'wb') as dest:
        dest.truncate(image_size)
        if disk_id is not None:
            mbr = bytearray(512)
            struct.pack_into('<I', mbr, 440, disk_id)
            for i, (active, kind, start, size) in enumerate([
                    (0x80, 0x0e if profile == 'pro-sd' else 0x0c, boot_at, boot_size),
                    (0, 0x83, root_at, root_sectors)]):
                struct.pack_into('<B3sB3sII', mbr, 446 + 16 * i, active, b'\xfe\xff\xff',
                                 kind, b'\xfe\xff\xff', start, size)
            mbr[510:512] = b'\x55\xaa'
            dest.write(mbr)
        for path, at in payloads:
            dest.seek(at)
            with open(path, 'rb') as source:
                shutil.copyfileobj(source, dest, 1048576)
        dest.flush()
        os.fsync(dest.fileno())
    for path, at in payloads:
        assert digest(path) == digest(output, at, path.stat().st_size), 'payload checksum mismatch'
    if profile == 'pro-emmc':
        fd = os.open(output, os.O_RDONLY)
        assert os.lseek(fd, 0, os.SEEK_DATA) >= 22806528, 'vendor boot prefix was written'
        os.close(fd)
    payload_sha256, image_sha256 = append_trailer(output, profile, image_size, build_id)
    file_sha256 = digest(output)
    manifest = dict(build_id=build_id, media=profile, image_size=image_size,
                    file_size=image_size + TRAILER_SIZE,
                    trailer_offset=image_size, trailer_size=TRAILER_SIZE,
                    metadata_version=TRAILER_VERSION,
                    payload_sha256=payload_sha256, image_sha256=image_sha256,
                    file_sha256=file_sha256,
                    partition_table=table, rootfstype='squashfs', overlay='shared-upper',
                    first_boot_partitions=4 if table == 'cmdline' else 2, target_partitions=4,
                    rootfs_bytes=root_bytes, rootfs_slot_bytes=slot_size * 512,
                    rootfs_sha256=digest(rootfs), boot_sha256=digest(boot),
                    userdata_firstboot='f2fs-preserve-existing-ext4-or-f2fs',
                    boot_start_sector=boot_at, boot_sectors=boot_size,
                    rootfs_a_start_sector=root_at,
                    installed_boot_sectors=target_boot_size,
                    installed_rootfs_a_start_sector=target_root_at,
                    rootfs_b_start_sector=root_b,
                    userdata_start_sector=data_at, bootloader_firmware='absent' if profile.startswith('pro-') else 'fip-in-fat')
    if disk_id:
        manifest['disk_id'] = '%08x' % disk_id
    else:
        manifest.update(bootloader_preserve_bytes=22806528, boot0='absent',
                        boot_selector_offset=156958720, rootfs_b_firstboot='copy-A',
                        blkdevparts='mmcblk0:128M@157024256(pro-boot),128M@291241984(pro-rootfs-a),128M(pro-rootfs-b),-(pro-userdata)')
    Path(str(output) + '.manifest').write_text(''.join('%s=%s\n' % pair for pair in sorted(manifest.items())))
    Path(str(output) + '.sha256').write_text('%s  %s\n' % (file_sha256, Path(output).name))

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('profile', 'boot', 'rootfs', 'output', 'build-id'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--raw', action='append', default=[], help='FILE@BYTE_OFFSET (eMMC only)')
    args = parser.parse_args()
    raw = [(Path(pair.rsplit('@', 1)[0]), int(pair.rsplit('@', 1)[1])) for pair in args.raw]
    build(args.profile, args.boot, args.rootfs, args.output, args.build_id, raw)

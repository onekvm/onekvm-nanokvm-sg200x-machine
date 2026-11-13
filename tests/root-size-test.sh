#!/bin/sh
# First boot copies only the used part of rootfs A: SquashFS bytes_used or the
# EROFS block count. Set ONEKVM_TEST_EROFS to also check a real image.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
. "$project/firstboot/15-nanokvm-firstboot"
nanokvm_die() { printf '%s\n' "$*" >"$work/died"; exit 7; }

# image NAME FS SIZE [BLKSZBITS] [INCOMPAT]
image() {
    python3 - "$work/$1" "$2" "$3" "${4:-12}" "${5:-0}" <<'PY'
import struct, sys
path, fs, size, bits, incompat = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
header = bytearray(2048)
if fs == 'squashfs':
    header[:4] = b'hsqs'
    struct.pack_into('<Q', header, 40, size)
elif fs == 'erofs':
    struct.pack_into('<I', header, 1024, 0xE0F5E1E2)
    header[1024 + 12] = bits
    header[1024 + 13] = 3  # sb_extslots shares the word with blkszbits
    struct.pack_into('<I', header, 1024 + 36, size >> bits)
    struct.pack_into('<I', header, 1024 + 80, incompat)
open(path, 'wb').write(header)
PY
}

expect_size() {
    rootfs_bytes_used=
    nanokvm_root_bytes "$work/$1" "$2"
    [ "$rootfs_bytes_used" = "$3" ] || {
        echo "$1: expected $3, got $rootfs_bytes_used" >&2
        exit 1
    }
}

expect_die() {
    rm -f "$work/died"
    if (nanokvm_root_bytes "$work/$1" "$2"); then
        echo "$1 was accepted" >&2
        exit 1
    fi
    grep -q "$3" "$work/died"
}

image squash squashfs 110305280
expect_size squash squashfs 110305280
image erofs erofs 87261184
expect_size erofs erofs 87261184
image erofs16k erofs $((4096 * 16384)) 14
expect_size erofs16k erofs $((4096 * 16384))

image big-squash squashfs 4294967296
expect_die big-squash squashfs 'SquashFS size is invalid'
image wide erofs 87261184 12 128
expect_die wide erofs '48-bit block counts'
image big-erofs erofs 4294967296
expect_die big-erofs erofs 'exceeds 4 GiB'
image plain none 0
expect_die plain erofs 'EROFS superblock is missing'
expect_die plain ext4 'neither SquashFS nor EROFS'

if [ -n "${ONEKVM_TEST_EROFS:-}" ]; then
    rootfs_bytes_used=
    nanokvm_root_bytes "$ONEKVM_TEST_EROFS" erofs
    [ "$rootfs_bytes_used" -eq "$(wc -c <"$ONEKVM_TEST_EROFS")" ]
fi
echo 'rootfs A size: SquashFS and EROFS headers, 48-bit and 4 GiB rejection PASS'

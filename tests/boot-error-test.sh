#!/bin/sh
# Paused-setup records are split files plus a zip. No block device is mounted.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin"
cc -Wall -Wextra -Werror -o "$work/bin/onekvm-boot-error-zip" \
    "$project/firstboot/onekvm-boot-error-zip.c"
export PATH=$work/bin:$PATH
. "$project/firstboot/15-nanokvm-firstboot"
check_zip() {
    python3 - "$1" "$2" <<'PY'
import pathlib, sys, zipfile
folder, archive = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
with zipfile.ZipFile(archive) as packed:
    bad = packed.testzip()
    if bad:
        raise SystemExit(bad)
    names = ["cmdline.txt", "dmesg.txt", "mounts.txt", "reason.txt", "storage.txt"]
    if sorted(packed.namelist()) != names:
        raise SystemExit(packed.namelist())
    for name in names:
        if packed.read(name) != (folder / name).read_bytes():
            raise SystemExit(name)
PY
}
fatal() { printf '%s\n' "$*" >"$work/fatal"; exit 7; }
nanokvm_splash() { printf '%s\n' "$*" >"$work/splash"; }
secret=onekvm-secret-not-for-the-card
export secret
printf 'set auth user test password %s\n' "$secret" >"$work/firstboot.txt"
mkdir -p "$work/sys/mmcblk2" "$work/sys/mmcblk2p1" "$work/boot"
printf '61022208\n' >"$work/sys/mmcblk2/size"
printf '2048\n' >"$work/sys/mmcblk2p1/start"
printf '262144\n' >"$work/sys/mmcblk2p1/size"
export ONEKVM_STORAGE_SYSFS=$work/sys
export ONEKVM_CMDLINE_FILE=$work/cmdline
export ONEKVM_BOOT_ERROR_DIR=$work/boot
printf 'root=PARTUUID=4f4b5650-03 onekvm.media=pro-sd rauc.slot=B\n' >"$work/cmdline"
disk_name=mmcblk2
disk=/dev/mmcblk2
loader=/dev/mmcblk2p1
storage_media=pro-sd
storage_slot=B
splash_language=zh
if (nanokvm_die "userdata contains ext4; refusing destructive migration"); then
    echo 'paused setup continued' >&2
    exit 1
fi
[ "$(cat "$work/splash")" = error-saved ]
[ "$(cat "$work/fatal")" = "OneKVM NanoKVM storage migration: userdata contains ext4; refusing destructive migration" ]
report=$work/boot/onekvm-boot-error/try-1
[ -f "$report/reason.txt" ]
[ -f "$report/cmdline.txt" ]
[ -f "$report/storage.txt" ]
[ -f "$report/mounts.txt" ]
[ -f "$report/dmesg.txt" ]
grep -q '安装已暂停。原因写在下面。这次记录在 SD 卡 onekvm-boot-error/try-1 文件夹，并打包为 try-1.zip。' "$report/reason.txt"
grep -q 'reason: userdata contains ext4; refusing destructive migration' "$report/reason.txt"
grep -q 'root=PARTUUID=4f4b5650-03 onekvm.media=pro-sd rauc.slot=B' "$report/cmdline.txt"
grep -q 'media: pro-sd' "$report/storage.txt"
grep -q 'slot: B' "$report/storage.txt"
grep -q 'partition: mmcblk2p1 start=2048 size=262144' "$report/storage.txt"
grep -q 'disk-size: 61022208' "$report/storage.txt"
check_zip "$report" "$work/boot/onekvm-boot-error/try-1.zip"
for record_file in "$report"/* "$work/boot/onekvm-boot-error/try-1.zip"; do
    if grep -q "$secret" "$record_file"; then
        echo 'boot error record contains a credential' >&2
        exit 1
    fi
done
[ ! -e "$work/boot/onekvm-boot-error.txt" ]
[ ! -e "$work/boot/onekvm-boot-error/.try-1" ]
if (nanokvm_die "rootfs A is not SquashFS; refusing initialization"); then
    echo 'second pause continued' >&2
    exit 1
fi
[ -d "$work/boot/onekvm-boot-error/try-2" ]
grep -q 'reason: rootfs A is not SquashFS; refusing initialization' \
    "$work/boot/onekvm-boot-error/try-2/reason.txt"
grep -q 'reason: userdata contains ext4; refusing destructive migration' "$report/reason.txt"
if grep -q 'refusing initialization' "$report/reason.txt"; then
    echo 'first record was replaced' >&2
    exit 1
fi
check_zip "$work/boot/onekvm-boot-error/try-2" "$work/boot/onekvm-boot-error/try-2.zip"
mkdir -p "$work/boot/onekvm-boot-error/try-4"
printf 'older\n' >"$work/boot/onekvm-boot-error/try-9.zip"
printf 'skip\n' >"$work/boot/onekvm-boot-error/try-notes"
mkdir -p "$work/boot/onekvm-boot-error/try-9-extra"
nanokvm_write_boot_error "$work/boot" 'unknown userdata size; refusing migration'
[ -d "$work/boot/onekvm-boot-error/try-10" ]
[ -f "$work/boot/onekvm-boot-error/try-10.zip" ]
[ ! -e "$work/boot/onekvm-boot-error/try-3" ]
grep -q 'reason: unknown userdata size; refusing migration' \
    "$work/boot/onekvm-boot-error/try-10/reason.txt"
splash_language=zh_tw
saved_path=$PATH
PATH=/usr/bin:/bin
nanokvm_write_boot_error "$work/boot" 'cannot identify boot medium'
PATH=$saved_path
[ -d "$work/boot/onekvm-boot-error/try-11" ]
[ ! -e "$work/boot/onekvm-boot-error/try-11.zip" ]
grep -q '安裝已暫停。原因寫在下面。這次記錄在 SD 卡 onekvm-boot-error/try-11 資料夾。' \
    "$work/boot/onekvm-boot-error/try-11/reason.txt"
if grep -q '\.zip' "$work/boot/onekvm-boot-error/try-11/reason.txt"; then
    echo 'missing zip was described as packed' >&2
    exit 1
fi

# A directory that cannot be written keeps the serial instruction.
rm -rf "$work/boot"
ONEKVM_BOOT_ERROR_DIR=$work/missing
splash_language=en
if (nanokvm_die "SD card was not found"); then
    echo 'missing card continued' >&2
    exit 1
fi
[ "$(cat "$work/splash")" = error ]
[ "$(cat "$work/fatal")" = "OneKVM NanoKVM storage migration: SD card was not found" ]

# A normal shell must not mount the machine disk while saving.
unset ONEKVM_BOOT_ERROR_DIR
loader=/dev/mmcblk0p1
nanokvm_save_boot_error 'must not touch the host disk'
[ "$nanokvm_boot_error_saved" = false ]
echo 'boot error record tests passed'

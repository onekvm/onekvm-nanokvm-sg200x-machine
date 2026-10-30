#!/bin/sh
# The paused-setup record is a normal file. No block device is mounted.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
. "$project/firstboot/15-nanokvm-firstboot"
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
report=$work/boot/onekvm-boot-error.txt
grep -q '安装已暂停。原因写在下面。本文件在 SD 卡启动分区，文件名 onekvm-boot-error.txt。' "$report"
grep -q 'reason: userdata contains ext4; refusing destructive migration' "$report"
grep -q 'cmdline: root=PARTUUID=4f4b5650-03 onekvm.media=pro-sd rauc.slot=B' "$report"
grep -q 'media: pro-sd' "$report"
grep -q 'slot: B' "$report"
grep -q 'partition: mmcblk2p1 start=2048 size=262144' "$report"
grep -q 'disk-size: 61022208' "$report"
if grep -q "$secret" "$report"; then
    echo 'boot error record contains a credential' >&2
    exit 1
fi
[ ! -e "$work/boot/.onekvm-boot-error.tmp" ]

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

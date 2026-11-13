#!/bin/sh
# A finished paused-setup record is one zip. A folder remains only if packing fails.
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
    python3 - "$@" <<'PY'
import os, sys, zipfile
archive, needles = sys.argv[1], sys.argv[2:]
names = ["cmdline.txt", "dmesg.txt", "mounts.txt", "reason.txt", "storage.txt"]
with zipfile.ZipFile(archive) as packed:
    bad = packed.testzip()
    if bad:
        raise SystemExit(bad)
    if sorted(packed.namelist()) != names:
        raise SystemExit(packed.namelist())
    blob = "\n".join(packed.read(name).decode() for name in names)
secret = os.environ.get("secret", "")
if secret and secret in blob:
    raise SystemExit("secret")
for needle in needles:
    if needle not in blob:
        raise SystemExit(needle)
PY
}
fatal() { printf '%s\n' "$*" >"$work/fatal"; exit 7; }
nanokvm_splash() {
    [ "$1" = --message ]
    printf '%s\n' "$2" >"$work/splash-message"
    shift 2
    printf '%s\n' "$*" >"$work/splash"
}
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
cmp "$work/fatal" "$work/splash-message"
records=$work/boot/onekvm-boot-error
check_zip "$records/try-1.zip" \
    '这次记录是 SD 卡 onekvm-boot-error/try-1.zip' \
    'reason: userdata contains ext4; refusing destructive migration' \
    'root=PARTUUID=4f4b5650-03 onekvm.media=pro-sd rauc.slot=B' \
    'media: pro-sd' \
    'slot: B' \
    'partition: mmcblk2p1 start=2048 size=262144' \
    'disk-size: 61022208'
[ ! -e "$records/try-1" ]
[ ! -e "$records/.try-1" ]
[ ! -e "$work/boot/onekvm-boot-error.txt" ]
if (nanokvm_die "rootfs A is neither SquashFS nor EROFS; refusing initialization"); then
    echo 'second pause continued' >&2
    exit 1
fi
[ ! -e "$records/try-2" ]
check_zip "$records/try-2.zip" 'reason: rootfs A is neither SquashFS nor EROFS; refusing initialization'
check_zip "$records/try-1.zip" 'reason: userdata contains ext4; refusing destructive migration'
if check_zip "$records/try-1.zip" 'refusing initialization' 2>/dev/null; then
    echo 'first record was replaced' >&2
    exit 1
fi
mkdir -p "$records/try-4"
printf 'older\n' >"$records/try-9.zip"
printf 'skip\n' >"$records/try-notes"
mkdir -p "$records/try-9-extra"
nanokvm_write_boot_error "$work/boot" 'unknown userdata size; refusing migration'
[ ! -e "$records/try-10" ]
[ ! -e "$records/try-3" ]
check_zip "$records/try-10.zip" 'reason: unknown userdata size; refusing migration'
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

# A failed final rename must retain the staged reason and report save failure.
PATH=/usr/bin:/bin
mv() { return 1; }
if (nanokvm_die "diagnostic folder rename failed"); then
    echo 'failed diagnostic save continued' >&2
    exit 1
fi
[ "$(cat "$work/splash")" = error ]
cmp "$work/fatal" "$work/splash-message"
[ ! -e "$records/try-12" ]
[ ! -e "$records/try-12.zip" ]
grep -q 'reason: diagnostic folder rename failed' "$records/.try-12/reason.txt"
unset -f mv
PATH=$saved_path
nanokvm_write_boot_error "$work/boot" 'save after a failed diagnostic rename'
check_zip "$records/try-13.zip" 'reason: save after a failed diagnostic rename'
grep -q 'reason: diagnostic folder rename failed' "$records/.try-12/reason.txt"

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

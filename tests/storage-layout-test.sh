#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
. "$root/storage/storage-layout.sh"
export ONEKVM_CMDLINE_FILE=$work/cmdline
mkdir -p "$work/sys/class/block"
export ONEKVM_STORAGE_SYSFS=$work/sys/class/block
blkid() { printf '%s\n' "${TEST_PARTITION:-/dev/mmcblk2p3}"; }
printf '%s\n' 'root=PARTUUID=4f4b5650-03 rauc.slot=B' >"$ONEKVM_CMDLINE_FILE"
onekvm_storage_select
[ "$storage_media:$disk:$root_slot_bytes:$root_b_start:$userdata_start" = pro-sd:/dev/mmcblk2:134217728:526336:788480 ]
printf '%s\n' 'root=PARTLABEL=pro-rootfs-a rauc.slot=A onekvm.media=pro-emmc' >"$ONEKVM_CMDLINE_FILE"
onekvm_storage_select
[ "$storage_media:$disk:$root_slot_bytes:$root_b_start:$userdata_start" = pro-emmc:/dev/mmcblk0:134217728:830976:1093120 ]
printf '%s\n' 'root=/dev/mmcblk0p3 rauc.slot=B' >"$ONEKVM_CMDLINE_FILE"
onekvm_storage_select
[ "$storage_media:$disk:$root_slot_bytes:$root_b_start:$userdata_start" = nanokvm:/dev/mmcblk0:134217728:528384:790528 ]
mkdir -p "$work/sys/class/block/mmcblk0p1"
printf '%s\n' 1 >"$work/sys/class/block/mmcblk0p1/start"
printf '%s\n' 65536 >"$work/sys/class/block/mmcblk0p1/size"
export ONEKVM_STORAGE_SYSFS=$work/sys/class/block
printf '%s\n' 'root=/dev/mmcblk0p3 rauc.slot=B' >"$ONEKVM_CMDLINE_FILE"
onekvm_storage_select
[ "$storage_media:$loader_size:$root_a_start:$root_b_start:$userdata_start" = nanokvm:65536:67584:331776:593920 ]
for cmd in 'root=/dev/sda2' 'root=PARTLABEL=pro-rootfs-a' 'root=PARTUUID=4f4b5650-02 onekvm.media=pro-emmc'; do
    printf '%s\n' "$cmd" >"$ONEKVM_CMDLINE_FILE"
    if onekvm_storage_select; then echo 'unsafe disk selection accepted' >&2; exit 1; fi
done
TEST_PARTITION='/dev/mmcblk0p2
/dev/mmcblk2p2'
printf '%s\n' 'root=PARTUUID=4f4b5650-02 rauc.slot=A' >"$ONEKVM_CMDLINE_FILE"
if onekvm_storage_select; then echo 'ambiguous cards accepted' >&2; exit 1; fi
echo "boot medium selection tests passed"
# Compact source offsets are distinct from the final installed layout.
unset TEST_PARTITION
printf '%s\n' 'root=/dev/mmcblk0p2 rauc.slot=A' >"$ONEKVM_CMDLINE_FILE"
mkdir -p "$work/sys/class/block/mmcblk0p2"
printf '%s\n' 73728 >"$work/sys/class/block/mmcblk0p1/size"
printf '%s\n' 75776 >"$work/sys/class/block/mmcblk0p2/start"
printf '%s\n' 100000 >"$work/sys/class/block/mmcblk0p2/size"
onekvm_storage_select
[ "$loader_size:$root_a_start" = 262144:264192 ]
onekvm_storage_carrier_geometry
[ "$carrier_boot_size:$carrier_root_start" = 73728:75776 ]
printf '%s\n' 73729 >"$work/sys/class/block/mmcblk0p1/size"
if onekvm_storage_carrier_geometry; then echo 'unaligned carrier accepted' >&2; exit 1; fi
printf '%s\n' 73728 >"$work/sys/class/block/mmcblk0p1/size"
mkdir "$work/sys/class/block/mmcblk0p3"
if onekvm_storage_carrier_geometry; then echo 'installed layout accepted as carrier' >&2; exit 1; fi
echo 'compact carrier selection tests passed'
rmdir "$work/sys/class/block/mmcblk0p3"
mkdir -p "$work/sys/class/block/mmcblk2p1" "$work/sys/class/block/mmcblk2p2"
printf '2048\n' >"$work/sys/class/block/mmcblk2p1/start"
printf '98304\n' >"$work/sys/class/block/mmcblk2p1/size"
printf '100352\n' >"$work/sys/class/block/mmcblk2p2/start"
printf '100000\n' >"$work/sys/class/block/mmcblk2p2/size"
TEST_PARTITION=/dev/mmcblk2p2
printf 'root=PARTUUID=4f4b5650-02 rauc.slot=A\n' >"$ONEKVM_CMDLINE_FILE"
onekvm_storage_select
[ "$loader_size:$root_a_start" = 262144:264192 ]
onekvm_storage_carrier_geometry
[ "$carrier_boot_start:$carrier_boot_size:$carrier_root_start" = 2048:98304:100352 ]
echo 'Pro SD compact carrier selection tests passed'

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
[ "$storage_media:$disk:$root_slot_bytes:$root_b_start:$userdata_start" = pro-emmc:/dev/mmcblk0:134217728:565248:827392 ]
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
[ -z "$disk$disk_name$loader$root_a$root_b$userdata" ]

# A known boot disk remains available to save a rejected slot's error. A later
# invalid selection must never reuse that device for diagnostic writes.
unset TEST_PARTITION
printf 'root=PARTUUID=4f4b5650-03 rauc.slot=A\n' >"$ONEKVM_CMDLINE_FILE"
if onekvm_storage_select; then echo 'conflicting slot accepted' >&2; exit 1; fi
[ "$disk:$loader" = /dev/mmcblk2:/dev/mmcblk2p1 ]
printf 'root=/dev/sda2\n' >"$ONEKVM_CMDLINE_FILE"
if onekvm_storage_select; then echo 'unknown root accepted' >&2; exit 1; fi
[ -z "$disk$disk_name$loader$root_a$root_b$userdata" ]

# The Go passes only rauc.slot; the device tree names the machine.
export ONEKVM_DT_COMPATIBLE_FILE=$work/compatible
printf 'sipeed,nanokvm-go\000axera,ax620q\000' >"$ONEKVM_DT_COMPATIBLE_FILE"
printf 'console=ttyS3,115200n8 rauc.slot=B onekvm.build-id=test\n' >"$ONEKVM_CMDLINE_FILE"
onekvm_storage_select
[ "$storage_media:$disk:$loader:$root_b:$userdata" = go-sd:/dev/mmcblk1:/dev/mmcblk1p1:/dev/mmcblk1p3:/dev/mmcblk1p4 ]
[ "$loader_start:$root_a_start:$root_b_start:$userdata_start" = 2048:264192:526336:788480 ]
[ "$storage_compatible" = 'OneKVM NanoKVM Go' ]
printf 'console=ttyS3,115200n8\n' >"$ONEKVM_CMDLINE_FILE"
if onekvm_storage_select; then echo 'Go without a slot accepted' >&2; exit 1; fi
printf 'rauc.slot=A onekvm.media=pro-sd\n' >"$ONEKVM_CMDLINE_FILE"
if onekvm_storage_select; then echo 'Go with another medium accepted' >&2; exit 1; fi
printf 'axera,ax620q\000' >"$ONEKVM_DT_COMPATIBLE_FILE"
for cmd in 'rauc.slot=A' 'root=/dev/mmcblk1p2 rauc.slot=A'; do
    printf '%s\n' "$cmd" >"$ONEKVM_CMDLINE_FILE"
    if onekvm_storage_select; then echo 'Go layout accepted on another machine' >&2; exit 1; fi
done

# PicoKVM: the same slot-only command line, its own geometry and ext4 userdata.
printf 'luckfox,pico-kvm\000luckfox,pico\000rockchip,rv1106\000' >"$ONEKVM_DT_COMPATIBLE_FILE"
printf 'console=ttyS0,115200 rootwait rauc.slot=A onekvm.build-id=test\n' >"$ONEKVM_CMDLINE_FILE"
onekvm_storage_select
[ "$storage_media:$disk:$loader:$root_a:$userdata" = picokvm-sd:/dev/mmcblk1:/dev/mmcblk1p1:/dev/mmcblk1p2:/dev/mmcblk1p4 ]
[ "$loader_start:$loader_size:$root_a_start:$root_slot_size:$root_b_start:$userdata_start" = 32768:262144:294912:524288:819200:1343488 ]
[ "$root_slot_bytes:$userdata_fs:$storage_compatible" = '268435456:ext4:OneKVM PicoKVM' ]
printf 'rauc.slot=B\n' >"$ONEKVM_CMDLINE_FILE"
onekvm_storage_select
[ "$storage_media:$root_b" = picokvm-sd:/dev/mmcblk1p3 ]
printf 'rauc.slot=A onekvm.media=go-sd\n' >"$ONEKVM_CMDLINE_FILE"
if onekvm_storage_select; then echo 'PicoKVM with another medium accepted' >&2; exit 1; fi
printf 'root=/dev/mmcblk1p2 rauc.slot=B\n' >"$ONEKVM_CMDLINE_FILE"
if onekvm_storage_select; then echo 'PicoKVM slot disagreement accepted' >&2; exit 1; fi
# Every other medium keeps f2fs for a factory reset.
printf 'sipeed,nanokvm-go\000' >"$ONEKVM_DT_COMPATIBLE_FILE"
printf 'rauc.slot=A\n' >"$ONEKVM_CMDLINE_FILE"
onekvm_storage_select
[ "$storage_media:$userdata_fs:$root_slot_size" = go-sd:f2fs:262144 ]
unset ONEKVM_DT_COMPATIBLE_FILE
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

# Lines are read like a one-line cat, without starting a process.
printf 'no newline' >"$work/line"
onekvm_read_line value "$work/line"
[ "$value" = 'no newline' ]
printf ' root=/dev/mmcblk0p2  rauc.slot=A \n' >"$work/line"
onekvm_read_line value "$work/line"
[ "$value" = ' root=/dev/mmcblk0p2  rauc.slot=A ' ]
value=stale
if onekvm_read_line value "$work/missing" 2>/dev/null; then echo 'missing file read' >&2; exit 1; fi
[ -z "$value" ]
# Boot runs these checks twice before the root switch; none starts cat.
mkdir -p "$work/bin"
printf '#!/bin/sh\necho "$*" >>"%s/cat-calls"\n' "$work" >"$work/bin/cat"
chmod +x "$work/bin/cat"
saved_path=$PATH
PATH=$work/bin:$PATH
cat /dev/null
[ -s "$work/cat-calls" ]
rm "$work/cat-calls"
onekvm_storage_select
onekvm_storage_carrier_geometry
mkdir -p "$work/sys/class/block/mmcblk0p1"
printf '1\n' >"$work/sys/class/block/mmcblk0p1/start"
printf '65536\n' >"$work/sys/class/block/mmcblk0p1/size"
printf 'root=/dev/mmcblk0p2 rauc.slot=A\n' >"$ONEKVM_CMDLINE_FILE"
onekvm_storage_select
PATH=$saved_path
[ "$storage_media:$storage_geometry" = nanokvm:nanokvm-legacy-32m ]
[ ! -e "$work/cat-calls" ]
echo 'storage checks read files without starting processes'

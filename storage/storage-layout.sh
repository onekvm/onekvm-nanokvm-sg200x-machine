#!/bin/sh
# Shared by firstboot, the running system and Recovery. Never guess a disk.
onekvm_storage_error() { echo "OneKVM storage: $*" >&2; return 1; }

onekvm_nanokvm_probe_geometry() {
    [ "$storage_media" = nanokvm ] || return 0
    storage_sysfs=${ONEKVM_STORAGE_SYSFS:-/sys/class/block}
    # New images use a 128 MiB loader. Keep recognizing the shipped 32 MiB
    # layout so RAUC and Recovery upgrades can continue to use old devices.
    [ -r "$storage_sysfs/${disk_name}p1/start" ] || return 0
    [ -r "$storage_sysfs/${disk_name}p1/size" ] || return 0
    old_start=$(cat "$storage_sysfs/${disk_name}p1/start") || return 0
    old_size=$(cat "$storage_sysfs/${disk_name}p1/size") || return 0
    case "$old_start:$old_size" in
        1:65536)
            loader_start=1; loader_size=65536; root_a_start=67584
            storage_geometry=nanokvm-legacy-32m ;;
        1:262144)
            storage_geometry=nanokvm-128m ;;
    esac
}

# Source geometry of a two-partition SD release carrier. Final A/B geometry
# stays separate; callers must also verify the bootgrow marker before writing.
onekvm_storage_carrier_geometry() {
    case "$storage_media" in nanokvm|pro-sd) ;; *) return 1 ;; esac
    storage_sysfs=${ONEKVM_STORAGE_SYSFS:-/sys/class/block}
    [ ! -e "$storage_sysfs/${disk_name}p3" ] &&
        [ ! -e "$storage_sysfs/${disk_name}p4" ] || return 1
    carrier_boot_start=$(cat "$storage_sysfs/${disk_name}p1/start") || return 1
    carrier_boot_size=$(cat "$storage_sysfs/${disk_name}p1/size") || return 1
    carrier_root_start=$(cat "$storage_sysfs/${disk_name}p2/start") || return 1
    carrier_root_size=$(cat "$storage_sysfs/${disk_name}p2/size") || return 1
    case "$carrier_boot_start:$carrier_boot_size:$carrier_root_start:$carrier_root_size" in
        *[!0-9:]*) return 1 ;;
    esac
    [ "$carrier_boot_start" -eq "$loader_start" ] &&
        [ "$carrier_boot_size" -ge 32768 ] && [ "$carrier_boot_size" -le 262144 ] &&
        [ "$((carrier_boot_size % 2048))" -eq 0 ] &&
        [ "$carrier_root_start" -eq "$(((carrier_boot_start + carrier_boot_size + 2047) / 2048 * 2048))" ] &&
        [ "$carrier_root_size" -gt 0 ] && [ "$carrier_root_size" -le "$root_slot_size" ]
}

onekvm_storage_select() {
    storage_root=; storage_media=; storage_slot=; storage_geometry=
    for storage_arg in $(cat "${ONEKVM_CMDLINE_FILE:-/proc/cmdline}"); do
        case "$storage_arg" in
            root=*) storage_root=${storage_arg#root=} ;;
            onekvm.media=*) storage_media=${storage_arg#onekvm.media=} ;;
            rauc.slot=A|rauc.slot=B) storage_slot=${storage_arg#rauc.slot=} ;;
        esac
    done
    case "$storage_root" in
        PARTUUID=4f4b5650-02|PARTUUID=4f4b5650-03)
            [ -z "$storage_media" ] || [ "$storage_media" = pro-sd ] || return 1
            storage_media=pro-sd
            storage_attempt=0
            disk_partition=
            while [ -z "$disk_partition" ] && [ "$storage_attempt" -lt 100 ]; do
                disk_partition=$(blkid -t "$storage_root" -o device 2>/dev/null || true)
                [ -n "$disk_partition" ] && break
                sleep 0.1
                storage_attempt=$((storage_attempt + 1))
            done
            # Multiple matching cards are ambiguous and must never be written.
            case "$disk_partition" in /dev/mmcblk[0-9]p[23]) ;; *) return 1 ;; esac
            disk=${disk_partition%p[23]}
            ;;
        PARTLABEL=pro-rootfs-a|PARTLABEL=pro-rootfs-b)
            [ "$storage_media" = pro-emmc ] || return 1
            disk=/dev/mmcblk0
            ;;
        /dev/mmcblk0p2|/dev/mmcblk0p3)
            [ -z "$storage_media" ] || [ "$storage_media" = nanokvm ] || return 1
            storage_media=nanokvm; disk=/dev/mmcblk0
            ;;
        *) onekvm_storage_error "unrecognized root device: $storage_root"; return 1 ;;
    esac
    case "$storage_root:$storage_slot" in
        PARTUUID=4f4b5650-02:A|PARTUUID=4f4b5650-03:B|PARTLABEL=pro-rootfs-a:A|PARTLABEL=pro-rootfs-b:B|/dev/mmcblk0p2:A|/dev/mmcblk0p3:B) ;;
        *) onekvm_storage_error "root device and boot slot disagree"; return 1 ;;
    esac
    disk_name=${disk##*/}
    loader=${disk}p1; root_a=${disk}p2; root_b=${disk}p3; userdata=${disk}p4
    # Cube and both Pro media use the same 128 MiB SquashFS slot capacity.
    root_slot_size=262144
    root_b_gap=0
    case "$storage_media" in
        nanokvm)
            loader_start=1; loader_size=262144; root_a_start=264192
            root_b_gap=2048
            userdata_uuid=4f4b564d-0000-4000-8000-000000000004
            storage_compatible='OneKVM NanoKVM' ;;
        pro-sd)
            loader_start=2048; loader_size=262144; root_a_start=264192
            userdata_uuid=4f4b5650-0000-4000-8000-000000000004
            storage_compatible='OneKVM NanoKVM Pro' ;;
        pro-emmc)
            loader_start=306688; loader_size=262144; root_a_start=568832
            userdata_uuid=4f4b5651-0000-4000-8000-000000000004
            storage_compatible='OneKVM NanoKVM Pro' ;;
    esac
    onekvm_nanokvm_probe_geometry
    root_b_start=$((root_a_start + root_slot_size + root_b_gap))
    userdata_start=$((root_b_start + root_slot_size))
    root_slot_bytes=$((root_slot_size * 512))
}

onekvm_storage_validate() {
    [ -b "$disk" ] || return 1
    storage_kind=$(cat "/sys/class/block/$disk_name/device/type") || return 1
    case "$storage_media:$storage_kind" in nanokvm:SD|pro-sd:SD|pro-emmc:MMC) ;; *) return 1 ;; esac
    for storage_number in 1 2 3 4; do
        storage_sys=/sys/class/block/${disk_name}p${storage_number}
        [ "$(cat "$storage_sys/partition")" = "$storage_number" ] || return 1
        storage_start=$(cat "$storage_sys/start") || return 1
        storage_size=$(cat "$storage_sys/size") || return 1
        case "$storage_number" in
            1) [ "$storage_start:$storage_size" = "$loader_start:$loader_size" ] || return 1 ;;
            2) [ "$storage_start:$storage_size" = "$root_a_start:$root_slot_size" ] || return 1 ;;
            3) [ "$storage_start:$storage_size" = "$root_b_start:$root_slot_size" ] || return 1 ;;
            4) [ "$storage_start" = "$userdata_start" ] && [ "$storage_size" -ge 65536 ] || return 1 ;;
        esac
    done
    if [ "$storage_media" = pro-emmc ]; then
        for storage_pair in '1 pro-boot' '2 pro-rootfs-a' '3 pro-rootfs-b' '4 pro-userdata'; do
            set -- $storage_pair
            grep -qx "PARTNAME=$2" "/sys/class/block/${disk_name}p$1/uevent" || return 1
        done
    fi
}

onekvm_storage_links() {
    onekvm_storage_select && onekvm_storage_validate || return 1
    for storage_pair in "$loader boot" "$root_a rootfs-a" "$root_b rootfs-b" "$userdata userdata"; do
        set -- $storage_pair
        ln -sfn "$1" "/dev/onekvm-$2" || return 1
    done
}

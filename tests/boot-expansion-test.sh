#!/bin/sh
# Exercise firstboot's real preservation/restore code on a simulated FAT mount.
# No block devices, real mounts or production paths are modified.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
. "$project/storage/storage-layout.sh"
. "$project/firstboot/15-nanokvm-firstboot"
export ONEKVM_STORAGE_SYSFS=$work/sys
export ONEKVM_CMDLINE_FILE=$work/cmdline
filesystem=$work/filesystem
migration_dir=$work/ram
mkdir -p "$filesystem" "$migration_dir" "$work/sys/mmcblk0p1" "$work/sys/mmcblk0p2"
printf 'root=/dev/mmcblk0p2 rauc.slot=A\n' >"$ONEKVM_CMDLINE_FILE"
printf '1\n' >"$work/sys/mmcblk0p1/start"
printf '73728\n' >"$work/sys/mmcblk0p1/size"
printf '75776\n' >"$work/sys/mmcblk0p2/start"
printf '100000\n' >"$work/sys/mmcblk0p2/size"
printf '128\n' >"$filesystem/bootgrow"
printf 'new account and network\n' >"$filesystem/firstboot.txt"
printf 'boot kernel a\n' >"$filesystem/boot.sd"
printf 'boot kernel b\n' >"$filesystem/boot-b.itb"
printf 'firmware\n' >"$filesystem/fip.bin"
printf 'boot environment\n' >"$filesystem/uEnv.txt"
(cd "$filesystem" && sha256sum firstboot.txt boot.sd boot-b.itb fip.bin uEnv.txt) >"$work/expected.sha256"
fatal() { echo "$*" >&2; exit 1; }
msg() { :; }
sfdisk() { printf '0x4f4b564d\n'; }
mount() {
    for mount_arg do mount_target=$mount_arg; done
    rmdir "$mount_target"
    ln -s "$filesystem" "$mount_target"
}
umount() { rm "$1"; mkdir "$1"; }
mkdir "$work/bin"
export work filesystem
cat >"$work/bin/mkfs.vfat" <<'MOCK'
#!/bin/sh
printf 'formatted\n' >"$work/formatted"
rm -f "$filesystem/"*
MOCK
chmod +x "$work/bin/mkfs.vfat"
export PATH=$work/bin:$PATH
onekvm_storage_select
nanokvm_inspect_carrier
[ "$grow_boot:$source_loader_size:$source_root_start:$loader_size:$root_a_start:$userdata_start" = true:73728:75776:262144:264192:790528 ]
nanokvm_copy_boot_to_ram
[ ! -e "$work/formatted" ]
nanokvm_restore_boot
[ -e "$work/formatted" ] && [ ! -e "$filesystem/bootgrow" ]
(cd "$filesystem" && sha256sum -c "$work/expected.sha256")
# A shipped 32 MiB layout without the new marker retains its geometry.
printf '65536\n' >"$work/sys/mmcblk0p1/size"
printf '67584\n' >"$work/sys/mmcblk0p2/start"
onekvm_storage_select
nanokvm_inspect_carrier
[ "$grow_boot:$loader_size:$root_a_start" = false:65536:67584 ]
# Existing A/B systems never enter boot expansion, even with a stale marker.
printf '128\n' >"$filesystem/bootgrow"
mkdir "$work/sys/mmcblk0p3" "$work/sys/mmcblk0p4"
nanokvm_inspect_carrier
[ "$grow_boot:$loader_size:$root_a_start" = false:65536:67584 ]
rmdir "$work/sys/mmcblk0p3" "$work/sys/mmcblk0p4"
# An unknown request must stop before any formatting.
printf '32\n' >"$filesystem/bootgrow"
rm "$work/formatted"
if (nanokvm_inspect_carrier); then echo 'accepted unknown boot size' >&2; exit 1; fi
[ ! -e "$work/formatted" ]
echo 'boot expansion preservation and legacy compatibility tests passed'

# Language is read as data; no other CLI statement is evaluated by the shell.
nanokvm_splash() { :; }
for language in en zh zh_tw; do
    printf 'set system language %s\nrequest system setup complete\n' "$language" >"$filesystem/firstboot.txt"
    nanokvm_read_splash_language
    [ "$splash_language" = "$language" ]
done
printf 'set system language en; touch %s\nset auth user test password $(touch %s)\n' "$work/executed" "$work/executed" >"$filesystem/firstboot.txt"
nanokvm_read_splash_language
[ "$splash_language" = en ] && [ ! -e "$work/executed" ]
rm "$filesystem/firstboot.txt"
nanokvm_read_splash_language
[ "$splash_language" = en ]
echo 'firstboot splash language and non-executable configuration tests passed'

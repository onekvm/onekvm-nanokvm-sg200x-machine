#!/bin/sh
# Run the real RAM-preservation and FAT-restore path on ordinary directories.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
. "$project/firstboot/15-nanokvm-firstboot"
fatal() { echo "$*" >&2; exit 1; }
msg() { :; }
sync() { :; }
mount() {
    for argument do destination=$argument; done
    rmdir "$destination"
    ln -s "$filesystem" "$destination"
}
umount() { rm "$1"; mkdir "$1"; }
mkdir "$work/bin"
cat >"$work/bin/mkfs.vfat" <<'SH'
#!/bin/sh
printf 'formatted\n' >"$case_dir/formatted"
rm -f "$filesystem/"*
SH
chmod +x "$work/bin/mkfs.vfat"
export PATH=$work/bin:$PATH
export case_dir filesystem
prepare() {
    case_dir=$work/$1
    filesystem=$case_dir/fat
    migration_dir=$case_dir/ram
    boot_mount=$migration_dir/mount
    boot_backup=$migration_dir/boot-files
    loader=/dev/unused
    storage_media=nanokvm
    grow_boot=true
    mkdir -p "$filesystem" "$migration_dir"
    printf '128\n' >"$filesystem/bootgrow"
    printf 'A\n' >"$filesystem/bootclone"
    printf 'kernel and initramfs\n' >"$filesystem/boot.sd"
    printf 'same-build\n' >"$filesystem/build-id-a"
    cp "$filesystem/build-id-a" "$filesystem/build-id-b"
    printf 'private firstboot settings\n' >"$filesystem/firstboot.txt"
    printf 'original uEnv\0' >"$filesystem/uEnv.txt"
    (cd "$filesystem" && sha256sum boot.sd build-id-a build-id-b firstboot.txt uEnv.txt) >"$case_dir/expected.sha256"
}

prepare compact
nanokvm_copy_boot_to_ram
[ ! -e "$case_dir/formatted" ] && [ ! -e "$filesystem/boot-b.itb" ]
cmp "$boot_backup/boot.sd" "$boot_backup/boot-b.itb"
(cd "$boot_backup" && sha256sum -c "$migration_dir/boot.sha256")
nanokvm_restore_boot
[ -e "$case_dir/formatted" ]
[ ! -e "$filesystem/bootgrow" ] && [ ! -e "$filesystem/bootclone" ]
cmp "$filesystem/boot.sd" "$filesystem/boot-b.itb"
(cd "$filesystem" && sha256sum -c "$case_dir/expected.sha256")

# Existing independent B payloads must survive expansion unchanged.
prepare legacy
rm "$filesystem/bootclone"
printf 'independent B kernel\n' >"$filesystem/boot-b.itb"
cp "$filesystem/boot-b.itb" "$case_dir/expected-b"
nanokvm_copy_boot_to_ram
nanokvm_restore_boot
cmp "$filesystem/boot-b.itb" "$case_dir/expected-b"

for damage in no-marker unknown-marker missing-a mismatched-id empty-b; do
    prepare "$damage"
    case "$damage" in
        no-marker) rm "$filesystem/bootclone" ;;
        unknown-marker) printf 'B\n' >"$filesystem/bootclone" ;;
        missing-a) rm "$filesystem/boot.sd" ;;
        mismatched-id) printf 'other-build\n' >"$filesystem/build-id-b" ;;
        empty-b) : >"$filesystem/boot-b.itb" ;;
    esac
    if (nanokvm_copy_boot_to_ram); then
        echo "accepted damaged carrier: $damage" >&2; exit 1
    fi
    [ ! -e "$case_dir/formatted" ]
done
echo 'compact NanoKVM boot cloning, preservation and pre-format failures passed'

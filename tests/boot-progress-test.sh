#!/bin/sh
# The initramfs draws the boot bar and hands its last value to the root system.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin"
cat >"$work/bin/onekvm-boot-splash" <<SH
#!/bin/sh
printf '%s\n' "\$*" >>"$work/calls"
SH
chmod +x "$work/bin/onekvm-boot-splash"
export PATH=$work/bin:$PATH
. "$project/firstboot/15-nanokvm-firstboot"
nanokvm_oled_state() { :; }

nanokvm_splash boot
splash_language=zh
nanokvm_splash install 3
nanokvm_splash_progress storage
nanokvm_splash boot
nanokvm_splash error
cat >"$work/expected" <<'OUT'
--language en --progress 5 boot
--language zh install 3
progress storage
--language zh --progress storage boot
--language zh error
OUT
cmp "$work/expected" "$work/calls"

# Units name checkpoints in boot order and finish before the screen service.
[ "$(ls "$project"/boot-splash/systemd/*.service | wc -l)" -eq 3 ]
names=$(sed -n '/boot_stages\[\] = {/,/};/p' "$project/boot-splash/onekvm-boot-splash.c" | grep -o '"[a-z]*"' | tr -d '"' | tr '\n' ' ')
[ "$names" = "storage early sysinit basic screen done " ]
for stage in early sysinit basic; do
    unit=$project/boot-splash/systemd/onekvm-boot-progress-$stage.service
    grep -qx "ExecStart=-/usr/sbin/onekvm-boot-splash progress $stage" "$unit"
    grep -qx 'Before=onekvm-device-screen-display.service' "$unit"
    grep -qx 'DefaultDependencies=no' "$unit"
    grep -qx 'RemainAfterExit=yes' "$unit"
    # Never hold up a target; the stages only order among themselves.
    ! grep -q '^Before=.*\.target' "$unit"
done
grep -qx 'After=sysinit.target onekvm-boot-progress-early.service' \
    "$project/boot-splash/systemd/onekvm-boot-progress-sysinit.service"
grep -qx 'After=basic.target onekvm-boot-progress-sysinit.service' \
    "$project/boot-splash/systemd/onekvm-boot-progress-basic.service"

# The initramfs module starts the animator in the background, after Recovery.
: >"$work/calls"
. "$project/boot-splash/initramfs/80-bootprogress"
module=80-bootprogress
module=${module#*-}
module=${module%%-*}
[ "$module" = bootprogress ]
# Recovery is 20-recovery; the root filesystem is mounted from 85 on.
[ "$(ls "$project/boot-splash/initramfs")" = 80-bootprogress ]
bootprogress_run
wait
[ "$(cat "$work/calls")" = animate ]

echo 'boot progress hand-off tests passed'

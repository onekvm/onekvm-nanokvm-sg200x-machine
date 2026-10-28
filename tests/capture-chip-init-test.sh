#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export ONEKVM_EDID_MODEL_PATH=$work/model
export ONEKVM_EDID_DEVICE=$work/device
export ONEKVM_EDID_POWER_DEVICE=$work/power
export ONEKVM_EDID_SNAPSHOT_DEVICE=$work/snapshot
export ONEKVM_EDID_CHIP_PATH=$work/chip_id
export ONEKVM_EDID_PRESET=$root/share/edid/pro/1080p60.edid.bin
export ONEKVM_EDID_STATE_DIR=$work/state
init=$root/src/onekvm-machine-nanokvm-pro-edid
printf 'Sipeed NanoKVM Pro (AX630C)\000' >"$work/model"
printf 'lt6911d\n' >"$work/chip_id"
printf off >"$work/power"
export CAPTURE_INIT_REAL_DD=$(command -v dd)
dd if=/dev/zero of="$work/device" bs=256 count=1 2>/dev/null
"$init"
cmp "$work/device" "$ONEKVM_EDID_PRESET"
test -f "$work/state/pro-default-initialized"
dd if=/dev/zero of="$work/zeros" bs=256 count=1 2>/dev/null
cmp "$work/zeros" "$work/state/before-pro-default.bin"
# A later user EDID must survive both subsequent boots and package upgrades.
cp "$work/zeros" "$work/device"
"$init"
cmp "$work/device" "$work/zeros"
# A failed write must preserve its backup and leave initialization retryable.
rm "$work/state/pro-default-initialized"
mkdir "$work/bin"
cat >"$work/bin/dd" <<'EOF'
#!/bin/sh
case " $* " in
    *" of=$ONEKVM_EDID_DEVICE "*) exit 1 ;;
esac
exec "$CAPTURE_INIT_REAL_DD" "$@"
EOF
chmod +x "$work/bin/dd"
if PATH="$work/bin:$PATH" "$init"; then
    echo "failed EDID write was accepted" >&2; exit 1
fi
test ! -e "$work/state/pro-default-initialized"
cmp "$work/zeros" "$work/state/before-pro-default.bin"
"$init"
cmp "$work/device" "$ONEKVM_EDID_PRESET"
# A stale first read after bridge reset must cause another read, not a write.
rm "$work/state/pro-default-initialized"
cp "$work/zeros" "$work/device"
export CAPTURE_INIT_REAL_CAT=$(command -v cat)
export CAPTURE_INIT_READS=$work/reads
export CAPTURE_INIT_WRITES=$work/writes
export CAPTURE_INIT_STALE=$work/zeros
cat >"$work/bin/dd" <<'EOF'
#!/bin/sh
if [ "$1" = "if=$ONEKVM_EDID_DEVICE" ]; then
    [ "$3" = bs=256 ] && [ "$4" = count=1 ] || exit 1
    n=0
    [ ! -f "$CAPTURE_INIT_READS" ] || read -r n <"$CAPTURE_INIT_READS"
    n=$((n + 1))
    echo "$n" >"$CAPTURE_INIT_READS"
    if [ "$n" -eq 2 ]; then
        shift
        exec "$CAPTURE_INIT_REAL_DD" "if=$CAPTURE_INIT_STALE" "$@"
    fi
fi
case " $* " in
    *" of=$ONEKVM_EDID_DEVICE "*) echo write >>"$CAPTURE_INIT_WRITES" ;;
esac
exec "$CAPTURE_INIT_REAL_DD" "$@"
EOF
chmod +x "$work/bin/dd"
PATH="$work/bin:$PATH" "$init"
test "$(cat "$work/reads")" -eq 3
test "$(cat "$work/writes")" = write
# Reads must come from Flash even when a different complete snapshot exists.
cp "$work/zeros" "$work/snapshot"
cp "$ONEKVM_EDID_PRESET" "$work/device"
ONEKVM_EDID_STATE_DIR="$work/direct-state" "$init"
test -f "$work/direct-state/pro-default-initialized"
test ! -e "$work/direct-state/before-pro-default.bin"
test "$(cat "$work/power")" = off
# A Flash read error must fail, without falling back to the valid snapshot
# or writing the power interface, EDID or a success marker.
cp "$ONEKVM_EDID_PRESET" "$work/snapshot"
cat >"$work/bin/dd" <<'EOF'
#!/bin/sh
case " $* " in
    *" if=$ONEKVM_EDID_DEVICE "*) exit 1 ;;
    *" of=$ONEKVM_EDID_DEVICE "*) echo "unexpected Flash write" >&2; exit 1 ;;
esac
exec "$CAPTURE_INIT_REAL_DD" "$@"
EOF
if PATH="$work/bin:$PATH" ONEKVM_EDID_STATE_DIR="$work/read-failure" "$init"; then
    echo "failed Flash read was accepted" >&2; exit 1
fi
test ! -e "$work/read-failure/pro-default-initialized"
test ! -e "$work/read-failure/before-pro-default.bin"
test "$(cat "$work/power")" = off
# A short Flash read must also fail, without programming or a false marker.
printf short >"$work/device"
if ONEKVM_EDID_STATE_DIR="$work/short-read" "$init"; then
    echo "short Flash read was accepted" >&2; exit 1
fi
test ! -e "$work/short-read/pro-default-initialized"
test ! -e "$work/short-read/before-pro-default.bin"
test "$(cat "$work/device")" = short
test "$(cat "$work/power")" = off
rm "$work/snapshot"
# UXC initializes Flash once; later boots never replay a RAM preference.
printf 'lt6911uxc\n' >"$work/chip_id"
export ONEKVM_EDID_SHADOW_DEVICE=$work/shadow
cp "$root/share/edid/pro/720p60.edid.bin" "$work/shadow"
cp "$work/shadow" "$work/shadow-sentinel"
cp "$work/zeros" "$work/device"
ONEKVM_EDID_STATE_DIR="$work/uxc-state" "$init"
cmp "$work/device" "$ONEKVM_EDID_PRESET"
test -f "$work/uxc-state/pro-default-initialized"
test ! -e "$work/uxc-state/active.bin"
cmp "$work/shadow" "$work/shadow-sentinel"
# An obsolete userspace preference must not replace a newer Flash value.
cp "$root/share/edid/pro/720p60.edid.bin" "$work/device"
cp "$ONEKVM_EDID_PRESET" "$work/uxc-state/active.bin"
ONEKVM_EDID_STATE_DIR="$work/uxc-state" "$init"
cmp "$work/device" "$work/shadow-sentinel"
cmp "$work/shadow" "$work/shadow-sentinel"
# Lack of temporary Shadow support does not disable permanent Flash setup.
cp "$work/zeros" "$work/device"
ONEKVM_EDID_SHADOW_DEVICE=/nonexistent ONEKVM_EDID_STATE_DIR="$work/uxc-flash-only" "$init"
cmp "$work/device" "$ONEKVM_EDID_PRESET"
# C manual EDID support must not enable automatic writes or a false marker.
printf 'lt6911c\n' >"$work/chip_id"
ONEKVM_EDID_DEVICE=/nonexistent ONEKVM_EDID_STATE_DIR="$work/c-state" "$init"
test ! -e "$work/c-state"
# A missing driver still fails; it is not an optional C capability.
rm "$work/chip_id"
if ONEKVM_EDID_STATE_DIR="$work/missing-id" "$init"; then
    echo "missing chip identity allowed EDID programming" >&2; exit 1
fi
test ! -e "$work/missing-id"
if ONEKVM_EDID_DEVICE=/nonexistent ONEKVM_EDID_STATE_DIR="$work/missing" "$init"; then
    echo "missing bridge interface was accepted" >&2; exit 1
fi
printf 'unknown\n' >"$work/chip_id"
if ONEKVM_EDID_STATE_DIR="$work/unknown" "$init"; then
    echo "unknown bridge chip was accepted" >&2; exit 1
fi
printf 'lt6911uxc\n' >"$work/chip_id"
# Another board must not read or write the Pro bridge interface.
printf 'NanoKVM Cube\000' >"$work/model"
ONEKVM_EDID_DEVICE=/nonexistent ONEKVM_EDID_STATE_DIR="$work/other" "$init"
test ! -e "$work/other"
# Hooks run in order; a failed firmware step must stop the EDID step.
export ONEKVM_CAPTURE_CHIP_INIT_DIR=$work/hooks
export CAPTURE_INIT_TEST_LOG=$work/log
mkdir "$work/hooks"
printf '#!/bin/sh\necho firmware >>"$CAPTURE_INIT_TEST_LOG"\nexit 1\n' >"$work/hooks/10-firmware"
printf '#!/bin/sh\necho edid >>"$CAPTURE_INIT_TEST_LOG"\n' >"$work/hooks/20-edid"
chmod +x "$work/hooks/10-firmware" "$work/hooks/20-edid"
if "$root/src/onekvm-capture-chip-init"; then
    echo "failed firmware hook was accepted" >&2; exit 1
fi
test "$(cat "$work/log")" = firmware
printf '#!/bin/sh\necho firmware >>"$CAPTURE_INIT_TEST_LOG"\n' >"$work/hooks/10-firmware"
: >"$work/log"
"$root/src/onekvm-capture-chip-init"
test "$(cat "$work/log")" = "$(printf 'firmware\nedid')"
echo "capture chip initialization tests passed"

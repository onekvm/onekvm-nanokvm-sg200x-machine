#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export ONEKVM_EDID_MODEL_PATH=$work/model
export ONEKVM_EDID_DEVICE=$work/device
export ONEKVM_EDID_CHIP_PATH=$work/chip_id
export ONEKVM_EDID_PRESET=$root/share/edid/pro/1080p60.edid.bin
export ONEKVM_EDID_STATE_DIR=$work/state
init=$root/src/onekvm-machine-nanokvm-pro-edid
printf 'Sipeed NanoKVM Pro (AX630C)\000' >"$work/model"
printf 'lt6911d\n' >"$work/chip_id"
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
exit 1
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
export CAPTURE_INIT_REAL_DD=$(command -v dd)
export CAPTURE_INIT_READS=$work/reads
export CAPTURE_INIT_WRITES=$work/writes
export CAPTURE_INIT_STALE=$work/zeros
cat >"$work/bin/cat" <<'EOF'
#!/bin/sh
if [ "$1" = "$ONEKVM_EDID_DEVICE" ]; then
    n=0
    [ ! -f "$CAPTURE_INIT_READS" ] || read -r n <"$CAPTURE_INIT_READS"
    n=$((n + 1))
    echo "$n" >"$CAPTURE_INIT_READS"
    [ "$n" -ne 2 ] || exec "$CAPTURE_INIT_REAL_CAT" "$CAPTURE_INIT_STALE"
fi
exec "$CAPTURE_INIT_REAL_CAT" "$@"
EOF
cat >"$work/bin/dd" <<'EOF'
#!/bin/sh
echo write >>"$CAPTURE_INIT_WRITES"
exec "$CAPTURE_INIT_REAL_DD" "$@"
EOF
chmod +x "$work/bin/cat" "$work/bin/dd"
PATH="$work/bin:$PATH" "$init"
test "$(cat "$work/reads")" -eq 3
test "$(cat "$work/writes")" = write
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

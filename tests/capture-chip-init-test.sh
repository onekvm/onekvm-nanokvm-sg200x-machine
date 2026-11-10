#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export ONEKVM_EDID_MODEL_PATH=$work/model
export ONEKVM_LT6911_PROC=$work/lt6911_info
export ONEKVM_RUN_DIR=$work/run
init=$root/src/onekvm-machine-nanokvm-pro-edid
mkdir "$work/lt6911_info"
printf 'lt6911d\n' >"$work/lt6911_info/chip_id"
dd if=/dev/zero of="$work/lt6911_info/edid" bs=256 count=1 2>/dev/null
cp "$work/lt6911_info/edid" "$work/edid-before"
# Pro boot publishes the driver information and never programs the bridge EDID.
printf 'Sipeed NanoKVM Pro (AX630C)\000' >"$work/model"
"$init" | grep -qx 'Pro capture chip: lt6911d'
test "$(readlink "$work/run/lt6911_info")" = "$work/lt6911_info"
cmp "$work/lt6911_info/edid" "$work/edid-before"
test ! -e "$work/state"
# An existing link is kept; a missing driver is not an initialization failure.
"$init" >/dev/null
ONEKVM_LT6911_PROC=$work/missing ONEKVM_RUN_DIR=$work/run-missing "$init" 2>/dev/null
test ! -e "$work/run-missing"
# Another board does not publish Pro driver paths.
printf 'NanoKVM Cube\000' >"$work/model"
ONEKVM_RUN_DIR=$work/other "$init" >/dev/null
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

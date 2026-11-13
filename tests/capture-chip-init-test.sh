#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
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

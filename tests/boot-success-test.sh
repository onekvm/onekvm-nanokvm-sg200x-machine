#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin" "$work/boot"
cat >"$work/bin/mountpoint" <<'MOCK'
#!/bin/sh
[ "$1" = -q ] && [ "$2" = "$ONEKVM_BOOT_DIR" ] && [ "$TEST_MOUNTED" = yes ]
MOCK
cat >"$work/bin/systemctl" <<'MOCK'
#!/bin/sh
[ "$1:$2" = is-active:--quiet ] || exit 1
case "$3" in
    onekvm.service) [ "$TEST_CORE_ACTIVE" = yes ] ;;
    onekvm-rauc-health.service) [ "$TEST_HEALTH_ACTIVE" = yes ] ;;
    *) exit 1 ;;
esac
MOCK
chmod +x "$work/bin/"*
export PATH=$work/bin:$PATH ONEKVM_BOOT_DIR=$work/boot
export TEST_MOUNTED=yes TEST_CORE_ACTIVE=yes TEST_HEALTH_ACTIVE=yes
counter=$ONEKVM_BOOT_DIR/boot_retry_time.txt
for missing in TEST_MOUNTED TEST_CORE_ACTIVE TEST_HEALTH_ACTIVE; do
    printf '1\n1\n' >"$counter"
    export "$missing=no"
    if sh "$root/rauc-health/clear-boot-retry"; then
        echo "cleared attempts while $missing was false" >&2; exit 1
    fi
    [ "$(wc -l <"$counter")" -eq 2 ]
    export "$missing=yes"
done
sh "$root/rauc-health/clear-boot-retry"
[ ! -e "$counter" ]
sh "$root/rauc-health/clear-boot-retry"
echo 'successful boot cleanup and failed-health preservation tests passed'

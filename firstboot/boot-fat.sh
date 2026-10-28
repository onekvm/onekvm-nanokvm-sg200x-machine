#!/bin/sh
# Called once, before firstboot's first FAT mount. The caller has selected and
# validated OneKVM storage geometry. Failure is diagnostic, never boot policy.
onekvm_boot_fat_check() (
    fat_state=${1:-/run/onekvm-boot-fat}
    [ ! -f "$fat_state/status" ] || return 0
    mkdir -p "$fat_state" || return 0
    printf 'failed\n' >"$fat_state/status" || return 0
    fat_result=failed
    fat_reason='check unavailable'
    fat_log=$fat_state/fsck.log
    # Compare device numbers, so an alias such as /dev/onekvm-boot cannot
    # hide an existing mount. Refuse repair of either partition or whole disk.
    if [ -b "$loader" ] && [ -b "$disk" ] &&
        fat_dev=$(cat "/sys/class/block/${loader##*/}/dev") &&
        fat_disk_dev=$(cat "/sys/class/block/${disk##*/}/dev") &&
        ! awk -v part="$fat_dev" -v disk="$fat_disk_dev" \
            '$3 == part || $3 == disk { found=1 } END { exit !found }' /proc/self/mountinfo &&
        fat_checker=$(command -v fsck.fat) && fat_timeout=$(command -v timeout); then
        # /init has not exported PATH yet. Pass absolute executable paths;
        # timeout's execvp fallback does not include /sbin on musl.
        fat_rc=0
        "$fat_timeout" -k 2 15 "$fat_checker" -n "$loader" >"$fat_log" 2>&1 || fat_rc=$?
        case "$fat_rc" in
            0) fat_result=clean; fat_reason='read-only check passed' ;;
            1)
                fat_reason='automatic repair failed'
                fat_rc=0
                "$fat_timeout" -k 2 15 "$fat_checker" -a "$loader" >>"$fat_log" 2>&1 || fat_rc=$?
                case "$fat_rc" in
                    0|1)
                        fat_reason='repair verification failed'
                        if "$fat_timeout" -k 2 15 "$fat_checker" -n "$loader" >>"$fat_log" 2>&1; then
                            fat_result=repaired; fat_reason='repair verified'
                        fi ;;
                esac ;;
            *) fat_reason="read-only check failed ($fat_rc)" ;;
        esac
    else
        fat_reason='device mounted, absent, or checker unavailable'
    fi
    printf '%s\n' "$fat_result" >"$fat_state/status"
    printf '%s\n' "$fat_reason" >"$fat_state/reason"
    fat_message="OneKVM: boot FAT $fat_result: $fat_reason; continuing boot"
    if [ -c /dev/kmsg ]; then
        printf '<6>%s\n' "$fat_message" >/dev/kmsg || true
    else
        printf '%s\n' "$fat_message"
    fi
    return 0
)

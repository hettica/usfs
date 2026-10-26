#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Exercise production device opens throughout repeated safe reloads.
# Usage: USFS_SOURCE_DIR=/path/to/source USFS_STRESS_CYCLES=12 bash tests/e2e/proxy_reload_stress.sh
set -u
. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

cycles=${USFS_STRESS_CYCLES:-12}
workers=${USFS_STRESS_WORKERS:-4}
if ! [[ "$cycles" =~ ^[1-9][0-9]*$ && "$workers" =~ ^[1-9][0-9]*$ ]]; then
    echo 'cycles and workers must be positive integers' >&2
    exit 2
fi
tap_plan "$((cycles + 1))"
base=${TMPDIR:-/tmp}/usfs-proxy-stress-$$
mkdir -p "$base" || exit 1
pids=
stop_file=
worker_finished()
{
    local worker_id=$1

    ! kill -0 "$worker_id" 2>/dev/null
}
stop_workers()
{
    [[ -n "$stop_file" ]] && : > "$stop_file"
    failed=0
    for pid in $pids; do
        if ! usfs_wait_until "stress worker $pid exit" 30 worker_finished "$pid"; then
            kill -TERM "$pid" 2>/dev/null || true
            if ! usfs_wait_until "stress worker $pid termination" 5 worker_finished "$pid"; then
                kill -KILL "$pid" 2>/dev/null || true
            fi
            failed=1
        fi
        if worker_finished "$pid"; then wait "$pid" || failed=1; else failed=1; fi
    done
    pids=
    stop_file=
    return "$failed"
}
cleanup()
{
    exit_status=$1
    stop_workers >/dev/null 2>&1 || true
    mkdev -l usfs0 >/dev/null 2>&1 || true
    if [[ "$exit_status" -eq 0 ]]; then
        rm -rf "$base"
    else
        echo "# retained stress diagnostics: $base"
    fi
}
trap 'cleanup $?' EXIT
start_workers()
{
    phase=$1
    stop_file=$base/stop-$phase
    i=1
    while [[ "$i" -le "$workers" ]]; do
        ready=$base/ready-$phase-$i
        count_file=$base/count-$phase-$i
        (
            count=0
            while [[ ! -e "$stop_file" ]]; do
                /usr/sbin/usfsctl status >/dev/null 2>&1 || true
                count=$((count + 1))
                [[ "$count" -ne 1 ]] || : > "$ready"
            done
            printf '%s\n' "$count" > "$count_file"
        ) &
        pids="$pids $!"
        i=$((i + 1))
    done
    i=1
    while [[ "$i" -le "$workers" ]]; do
        usfs_wait_until "worker $phase/$i entered an open attempt" 20 \
            test -e "$base/ready-$phase-$i" || return 1
        i=$((i + 1))
    done
}
check_workers()
{
    phase=$1
    i=1
    while [[ "$i" -le "$workers" ]]; do
        count=$(cat "$base/count-$phase-$i" 2>/dev/null) || return 1
        [[ "$count" -gt 0 ]] || return 1
        i=$((i + 1))
    done
}
module_instances()
{
    local module_name=$1

    genkex | awk -v path="/usr/lib/drivers/usfs/$module_name" \
        '$NF == path { count++ } END { print count + 0 }'
}

status=$(/usr/sbin/usfsctl kexts) || exit 1
proxy_id=$(printf '%s\n' "$status" | sed -n 's/^proxy: .*module=\([^ ]*\).*/\1/p')
if printf '%s\n' "$status" | grep -q '^proxy: loaded=yes' &&
   printf '%s\n' "$status" | grep -q '^implementation: loaded=yes' &&
   printf '%s\n' "$status" | grep -q 'variant=release' &&
   [[ "$(module_instances usfs.kext)" -eq 1 ]] &&
   [[ "$(module_instances usfs_proxy.kext)" -eq 1 ]] &&
   [[ -n "$proxy_id" ]]; then result=0; else result=1; fi
tap_ok "$result" 'production modules loaded before open and reload stress'
[[ "$result" -eq 0 ]] || exit 1

cycle=1
while [[ "$cycle" -le "$cycles" ]]; do
    result=0
    start_workers "$cycle-open" || result=1
    if [[ "$result" -eq 0 ]]; then
        rmdev -l usfs0 >"$base/rmdev-$cycle.log" 2>&1 || true
    fi
    stop_workers || result=1
    check_workers "$cycle-open" || result=1
    if lsdev -l usfs0 -F status 2>/dev/null | grep -q Available; then
        rmdev -l usfs0 >>"$base/rmdev-$cycle.log" 2>&1 || result=1
    fi
    # AIX may retain a zero-reference unloaded kext in genkex until the next
    # loader cleanup. Force that documented cleanup before asserting that no
    # implementation load or use reference survived the unload.
    /usr/sbin/slibclean >"$base/slibclean-$cycle.log" 2>&1 || result=1
    genkex -d >"$base/genkex-$cycle-unloaded.txt"
    status=$(/usr/sbin/usfsctl kexts) || result=1
    printf '%s\n' "$status" | grep -q '^implementation: loaded=no' || result=1
    [[ "$(module_instances usfs.kext)" -eq 0 ]] || result=1
    [[ "$(module_instances usfs_proxy.kext)" -eq 1 ]] || result=1
    start_workers "$cycle-reload" || result=1
    mkdev -l usfs0 >"$base/mkdev-$cycle.log" 2>&1 || result=1
    genkex -d >"$base/genkex-$cycle-reloaded.txt"
    stop_workers || result=1
    check_workers "$cycle-reload" || result=1
    status=$(/usr/sbin/usfsctl kexts) || result=1
    new_proxy_id=$(printf '%s\n' "$status" | sed -n 's/^proxy: .*module=\([^ ]*\).*/\1/p')
    [[ "$proxy_id" = "$new_proxy_id" ]] || result=1
    printf '%s\n' "$status" | grep -q '^implementation: loaded=yes' || result=1
    [[ "$(module_instances usfs.kext)" -eq 1 ]] || result=1
    [[ "$(module_instances usfs_proxy.kext)" -eq 1 ]] || result=1
    tap_ok "$result" "cycle $cycle retains proxy identity and reloads after concurrent opens"
    [[ "$result" -eq 0 ]] || exit 1
    cycle=$((cycle + 1))
done
tap_finish

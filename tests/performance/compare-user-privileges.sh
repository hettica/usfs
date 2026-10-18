#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Purpose: Compare root and ordinary-user data I/O with the same calibration binary.
# Usage: bash compare-user-privileges.sh source build output-directory mountpoint
set -eu
export PATH=/opt/freeware/bin:/usr/bin:/usr/sbin:/etc
readonly source_root=$1
readonly build_root=$2
readonly output_root=$3
readonly mountpoint=$4
readonly application_uid=10000
[[ "$(uname -s)" = AIX ]]
[[ "$(id -u)" -eq 0 ]]
cmp "$build_root/bin/usfs.kext" /usr/lib/drivers/usfs/usfs.kext
if mount | awk '$3 == "usfs" { found=1 } END { exit !found }'; then
    echo 'Failed to reserve identity campaign: another USFS mount exists' >&2
    exit 1
fi

run_identity()
{
    local name=$1 instrumented=$2 rounds=$3 suite=$4
    echo "Starting $name $(date -u)"
    perl "$source_root/tests/performance/enforce-timeout.pl" 600 \
        "$build_root/testing/bin/usfs_overhead" "$mountpoint" "$output_root/$name" \
        8 0 "$instrumented" 0 "$rounds" "$suite" > "$output_root/$name.log" 2>&1 &
    local benchmark_pid=$!
    while kill -0 "$benchmark_pid" 2>/dev/null; do
        date -u
        tail -n 2 "$output_root/$name.log"
        sleep 20
    done
    wait "$benchmark_pid"
    tail -n 7 "$output_root/$name.log"
    usfsctl status > "$output_root/$name-status.txt" 2>&1
}

export USFS_OVERHEAD_UID=$application_uid
run_identity identity-smoke 1 1 data-smoke
unset USFS_OVERHEAD_UID
run_identity data-root-reference 0 3 data
export USFS_OVERHEAD_UID=$application_uid
run_identity data-user-uninstrumented 0 3 data
run_identity data-user-instrumented 1 3 data
unset USFS_OVERHEAD_UID
echo "Completed identity campaign $(date -u)"

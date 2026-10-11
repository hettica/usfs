#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Purpose: Run an isolated backend-neutral campaign directly on AIX.
# Usage: bash run-benchmarks.sh source build output-directory empty-mountpoint
set -eu
export PATH=/opt/freeware/bin:/usr/bin:/usr/sbin:/etc

readonly source_root=$1
readonly build_root=$2
readonly output_root=$3
readonly mountpoint=$4
readonly sample_deadline_seconds=900
readonly benchmark="$build_root/testing/bin/usfs_overhead"

test "$(uname -s)" = AIX
test "$(id -u)" -eq 0
test -d "$mountpoint"
mkdir -p "$output_root"
cmp "$build_root/bin/usfs.kext" /usr/lib/drivers/usfs/usfs.kext
if mount | awk '$3 == "usfs" { found=1 } END { exit !found }'; then
    echo 'Failed to reserve benchmark host: another USFS mount exists' >&2
    exit 1
fi

run_sample()
{
    local name=$1 pool=$2 delay=$3 instrumented=$4 fallback=$5 rounds=$6 suite=$7
    echo "Starting $name $(date -u)"
    perl "$source_root/tests/performance/enforce-timeout.pl" "$sample_deadline_seconds" \
        "$benchmark" "$mountpoint" "$output_root/$name" \
        "$pool" "$delay" "$instrumented" "$fallback" "$rounds" "$suite" \
        > "$output_root/$name.log" 2>&1 &
    local benchmark_pid=$!
    while kill -0 "$benchmark_pid" 2>/dev/null; do
        date -u
        tail -n 2 "$output_root/$name.log"
        sleep 20
    done
    local rc=0
    wait "$benchmark_pid" || rc=$?
    tail -n 8 "$output_root/$name.log"
    usfsctl status > "$output_root/$name-status.txt" 2>&1
    if test "$rc" -ne 0; then
        echo "Failed benchmark $name: exit status $rc" >&2
        return "$rc"
    fi
    if mount | awk -v target="$mountpoint" '$2 == target { found=1 } END { exit !found }'; then
        echo "Failed to clean benchmark mount $mountpoint" >&2
        return 1
    fi
}

{
    date -u
    hostname
    uname -a
    lslpp -L bos.rte
    lparstat -i
    vmstat -v
    svmon -G
    lsattr -El usfs0
    usfsctl status
    cksum "$benchmark" "$build_root/bin/libusfs.a" /usr/lib/drivers/usfs/usfs.kext
    cat "$build_root/CMakeFiles/_usfs_testing_usfs_overhead.dir/flags.make"
    cat "$build_root/src/client/CMakeFiles/_usfs_client_objects.dir/flags.make"
} > "$output_root/environment.txt" 2>&1

run_sample smoke 8 0 1 0 1 smoke
run_sample metadata-uninstrumented 8 0 0 0 5 metadata
run_sample metadata-instrumented 8 0 1 0 5 metadata
run_sample metadata-delay1000 8 1000 1 0 1 metadata
run_sample create-fallback 8 0 1 1 5 metadata
run_sample data-uninstrumented 8 0 0 0 5 data
run_sample data-instrumented 8 0 1 0 5 data
run_sample data-delay100 8 100 1 0 3 data
run_sample data-delay1000 8 1000 1 0 3 data
run_sample data-pool1 1 0 0 0 3 data
usfsctl status > "$output_root/final-status.txt" 2>&1
echo "Completed production overhead campaign $(date -u)"

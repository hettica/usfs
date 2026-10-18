#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Purpose: Measure native FID/VGET diagnostics using the test-only AIX control.
# Usage: bash measure-file-identifiers.sh source build output-directory mountpoint
set -eu
export PATH=/opt/freeware/bin:/usr/bin:/usr/sbin:/etc
readonly source_root=$1
readonly build_root=$2
readonly output_root=$3
readonly mountpoint=$4

[[ "$(uname -s)" = AIX ]]
[[ "$(id -u)" -eq 0 ]]
cmp "$build_root/bin/usfs.kext" /usr/lib/drivers/usfs/usfs.kext
if mount | awk '$3 == "usfs" { found=1 } END { exit !found }'; then
    echo 'Failed to reserve identifier diagnostics: another USFS mount exists' >&2
    exit 1
fi

restore_release()
{
    local original_status=$?
    trap - EXIT HUP INT TERM
    local restore_status=0
    rmdev -l usfs0 >> "$output_root/identifier-restore.log" 2>&1 || restore_status=$?
    if [[ "$restore_status" -eq 0 ]]; then
        bash "$source_root/scripts/aix/install-variant.sh" release "$source_root" "$build_root" \
            >> "$output_root/identifier-restore.log" 2>&1 || restore_status=$?
    fi
    if [[ "$restore_status" -eq 0 ]]; then
        bash "$source_root/scripts/aix/setup.sh" >> "$output_root/identifier-restore.log" 2>&1 || restore_status=$?
    fi
    cmp "$build_root/bin/usfs.kext" /usr/lib/drivers/usfs/usfs.kext || restore_status=1
    usfsctl status >> "$output_root/identifier-restore.log" 2>&1 || restore_status=1
    [[ "$restore_status" -eq 0 ]] || exit "$restore_status"
    exit "$original_status"
}

trap restore_release EXIT HUP INT TERM
rmdev -l usfs0 > "$output_root/identifier-install.log" 2>&1
bash "$source_root/scripts/aix/install-variant.sh" testing "$source_root" "$build_root" \
    >> "$output_root/identifier-install.log" 2>&1
bash "$source_root/scripts/aix/setup.sh" >> "$output_root/identifier-install.log" 2>&1
cksum /usr/lib/drivers/usfs/usfs.kext > "$output_root/identifier-kernel.txt"
perl "$source_root/tests/performance/enforce-timeout.pl" 180 \
    "$build_root/testing/bin/usfs_overhead" "$mountpoint" "$output_root/identifiers" \
    8 0 1 0 5 identifiers > "$output_root/identifiers.log" 2>&1 &
diagnostic_pid=$!
while kill -0 "$diagnostic_pid" 2>/dev/null; do
    date -u
    tail -n 2 "$output_root/identifiers.log"
    sleep 10
done
wait "$diagnostic_pid"
tail -n 12 "$output_root/identifiers.log"
usfsctl status > "$output_root/identifiers-status.txt" 2>&1

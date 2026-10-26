#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Verify safe implementation unload and reload with production binaries.
# Usage: USFS_SOURCE_DIR=/path/to/source bash tests/e2e/production_proxy.sh
set -u
. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"
tap_plan 8
backend=0
mountpoint=
base=${TMPDIR:-/tmp}/usfs-production-proxy-$$
mkdir -p "$base" || exit 1
printf 'resident proxy production I/O\n' > "$base/expected"
mounted()
{
    mount 2>/dev/null | awk '$3 == "usfs" { print $2 }' | grep -qx "$mountpoint"
}
mount_gone()
{
    ! mounted
}
stop_backend()
{
    if [[ "$backend" -ne 0 ]]; then
        kill -TERM "$backend" 2>/dev/null || true
        usfs_wait_until 'backend unmount' 30 mount_gone || return 1
        wait "$backend" 2>/dev/null || true
        backend=0
    fi
    return 0
}
cleanup()
{
    stop_backend >/dev/null 2>&1 || true
    if ! lsdev -l usfs0 2>/dev/null | grep -q Available; then
        mkdev -l usfs0 >/dev/null 2>&1 || true
    fi
    rm -rf "$base"
}
trap cleanup EXIT
start_backend()
{
    /usr/sbin/usfs_memfs --size=8 </dev/null >"$base/backend.log" 2>&1 &
    backend=$!
    mountpoint=/mnt/$backend/memfs
    usfs_wait_until 'production memfs mount' 30 mounted
}
verified_io()
{
    cp "$base/expected" "$mountpoint/verify" &&
        cmp "$base/expected" "$mountpoint/verify"
}
status=$(/usr/sbin/usfsctl kexts) || exit 1
proxy_id=$(printf '%s\n' "$status" | sed -n 's/^proxy: .*module=\([^ ]*\).*/\1/p')
if printf '%s\n' "$status" | grep -q 'variant=release' &&
   printf '%s\n' "$status" | grep -q '^implementation: loaded=yes'; then result=0; else result=1; fi
tap_ok "$result" 'production proxy and implementation are loaded'
start_backend; tap_ok "$?" 'production backend mounts'
verified_io; tap_ok "$?" 'production filesystem I/O is verified'
if rmdev -l usfs0 >"$base/busy.log" 2>&1; then result=1; else result=0; fi
tap_ok "$result" 'live mount rejects implementation unload'
stop_backend; result=$?
if [[ "$result" -eq 0 ]]; then rmdev -l usfs0 >"$base/unload.log" 2>&1 || result=1; fi
if [[ "$result" -eq 0 ]]; then
    status=$(/usr/sbin/usfsctl kexts) || result=1
    printf '%s\n' "$status" | grep -q '^implementation: loaded=no' || result=1
fi
tap_ok "$result" 'complete teardown unloads implementation'
mkdev -l usfs0 >"$base/reload.log" 2>&1; result=$?
status=$(/usr/sbin/usfsctl kexts) || result=1
new_proxy_id=$(printf '%s\n' "$status" | sed -n 's/^proxy: .*module=\([^ ]*\).*/\1/p')
[[ "$proxy_id" = "$new_proxy_id" ]] || result=1
tap_ok "$result" 'implementation reload retains resident proxy module identity'
start_backend; result=$?
if [[ "$result" -eq 0 ]]; then verified_io || result=1; fi
tap_ok "$result" 'remounted production filesystem passes I/O'
stop_backend; result=$?
[[ "$result" -eq 0 ]] && /usr/sbin/usfsctl status >"$base/final-status.log" 2>&1 || result=1
tap_ok "$result" 'workers stop and production service remains healthy'
tap_finish

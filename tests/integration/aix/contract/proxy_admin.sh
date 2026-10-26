#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Check inspection and explicit maintenance safety at the CLI boundary.
# Usage: USFS_SOURCE_DIR=/path/to/source bash tests/integration/aix/contract/proxy_admin.sh
set -u
. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"
tap_plan 9
base=${TMPDIR:-/tmp}/usfs-proxy-admin-$$
mkdir -p "$base" || exit 1
cleanup()
{
    mkdev -l usfs0 >/dev/null 2>&1 || true
    rm -rf "$base"
}
trap cleanup EXIT

/usr/sbin/usfsctl kexts >"$base/both" 2>&1
result=$?
grep -q '^proxy: loaded=yes' "$base/both" || result=1
grep -q '^implementation: loaded=yes' "$base/both" || result=1
tap_ok "$result" 'kexts reports both loaded extensions'

/usr/sbin/usfsctl status >"$base/status" 2>&1
result=$?
grep -q '^proxy: loaded=yes' "$base/status" || result=1
grep -q '^implementation: loaded=yes' "$base/status" || result=1
tap_ok "$result" 'status includes both extensions and filesystem health'

if /usr/sbin/usfs_proxy_testctl direct-term >"$base/direct-term" 2>&1; then result=0; else result=1; fi
/usr/sbin/usfsctl kexts >"$base/after-direct-term" 2>&1 || result=1
grep -q '^proxy: loaded=yes .* state=1 ' "$base/after-direct-term" || result=1
grep -q '^implementation: loaded=yes' "$base/after-direct-term" || result=1
tap_ok "$result" 'direct implementation CFG_TERM is rejected while attached'

if /usr/sbin/usfsctl proxy unload --unsafe </dev/null >"$base/refused" 2>&1; then result=1; else result=0; fi
grep -q 'Noninteractive use requires' "$base/refused" || result=1
tap_ok "$result" 'unattended unsafe removal requires extra confirmation flag'

if /usr/sbin/usfsctl proxy unload --unsafe --confirm-unsafe-unload \
    >"$base/busy" 2>&1; then result=1; else result=0; fi
grep -q 'busy' "$base/busy" || result=1
tap_ok "$result" 'maintenance removal refuses attached implementation'

if su nobody -c '/usr/sbin/usfsctl proxy unload --unsafe --confirm-unsafe-unload' \
    >"$base/nonroot" 2>&1; then result=1; else result=0; fi
grep -q 'requires root' "$base/nonroot" || result=1
tap_ok "$result" 'maintenance removal requires root'

usfs_coverage_checkpoint proxy-admin-pre-term || exit 1
usfs_coverage_expect_export proxy-admin-term || exit 1
USFS_COVERAGE_EVENT=proxy-admin-term rmdev -l usfs0 >"$base/unload" 2>&1
result=$?
if [ "$result" -eq 0 ]; then
    /usr/sbin/usfsctl kexts >"$base/detached" 2>&1 || result=1
    grep -q '^proxy: loaded=yes' "$base/detached" || result=1
    grep -q '^implementation: loaded=no' "$base/detached" || result=1
fi
tap_ok "$result" 'kexts inspects resident proxy without status device'

if /usr/sbin/usfsctl status >"$base/offline" 2>&1; then result=1; else result=0; fi
tap_ok "$result" 'status retains unavailable-service exit behavior'

mkdev -l usfs0 >"$base/reload" 2>&1
result=$?
/usr/sbin/usfsctl kexts >"$base/reloaded" 2>&1 || result=1
grep -q '^proxy: loaded=yes' "$base/reloaded" || result=1
grep -q '^implementation: loaded=yes' "$base/reloaded" || result=1
tap_ok "$result" 'implementation reload restores inspection and filesystem service'
tap_finish

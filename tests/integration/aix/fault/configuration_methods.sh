#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Prove userspace configuration transactions report failures, reconcile
#   partial state on retry, and remain idempotent. Testing methods only.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/fault/configuration_methods.sh

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

tap_plan 10

if [ -x /usr/sbin/usfs_coverage ]; then
    for name in \
        "post-load configure rollback" \
        "post-device configure rollback" \
        "post-CFG_TERM retry" \
        "post-device-cleanup retry" \
        "post-unload retry" \
        "post-vfs-unregister retry" \
        "idempotent direct methods" \
        "failed ODM rollback retains ownership" \
        "lifecycle query error retains ownership" \
        "stale VFS registry reconciliation"
    do
        USFS_TAP_EXECUTED=$((USFS_TAP_EXECUTED + 1))
        echo "ok $USFS_TAP_EXECUTED - $name # SKIP method fault injection is covered by the non-coverage AIX gate"
    done
    tap_finish
    exit $?
fi

vfs_count()
{
    awk '$1 == "usfs" { count += 1 } END { print count + 0 }' /etc/vfs
}

is_available()
{
    [ "$(lsdev -l usfs0 -F status 2>/dev/null)" = "Available" ] &&
    [ -c /dev/usfs0 ] && [ "$(vfs_count)" -eq 1 ] &&
    /usr/sbin/usfs_testctl info >/dev/null 2>&1
}

is_defined()
{
    [ "$(lsdev -l usfs0 -F status 2>/dev/null)" = "Defined" ] &&
    [ ! -e /dev/usfs0 ] && [ "$(vfs_count)" -eq 0 ]
}

restore_available()
{
    /usr/lib/methods/usfs/ucfgusfs -l usfs0 >/dev/null 2>&1 || true
    mkdev -l usfs0 >/dev/null 2>&1
}

configure_failure_case()
{
    fault=$1
    name=$2
    rmdev -l usfs0 >/dev/null 2>&1 || true
    if USFS_TEST_METHOD_FAULT="$fault" mkdev -l usfs0 \
        >/tmp/usfs-method-config.log 2>&1; then
        tap_diag "$name unexpectedly reported success"
        restore_available
        tap_ok 1 "$name"
        return
    fi
    if is_defined && mkdev -l usfs0 >/dev/null 2>&1 && is_available; then
        tap_ok 0 "$name"
    else
        sed 's/^/# /' /tmp/usfs-method-config.log 2>/dev/null
        restore_available
        tap_ok 1 "$name"
    fi
}

unconfigure_failure_case()
{
    fault=$1
    name=$2
    restore_available || true
    if USFS_TEST_METHOD_FAULT="$fault" rmdev -l usfs0 \
        >/tmp/usfs-method-unconfig.log 2>&1; then
        tap_diag "$name unexpectedly reported success"
        restore_available
        tap_ok 1 "$name"
        return
    fi
    if rmdev -l usfs0 >/dev/null 2>&1 && is_defined &&
       mkdev -l usfs0 >/dev/null 2>&1 && is_available; then
        tap_ok 0 "$name"
    else
        sed 's/^/# /' /tmp/usfs-method-unconfig.log 2>/dev/null
        restore_available
        tap_ok 1 "$name"
    fi
}

configure_failure_case 2 "post-load configure failure rolls back and reports failure"
configure_failure_case 3 "post-device configure failure rolls back and reports failure"
unconfigure_failure_case 4 "post-CFG_TERM failure is retryable through rmdev"
unconfigure_failure_case 5 "post-device-cleanup failure is retryable through rmdev"
unconfigure_failure_case 6 "post-unload failure is retryable through rmdev"
unconfigure_failure_case 7 "post-vfs-unregister failure is retryable through rmdev"

idempotent_failure=0
/usr/lib/methods/usfs/cfgusfs -l usfs0 >/dev/null 2>&1 || idempotent_failure=1
/usr/lib/methods/usfs/cfgusfs -l usfs0 >/dev/null 2>&1 || idempotent_failure=1
[ "$(vfs_count)" -eq 1 ] || idempotent_failure=1
/usr/lib/methods/usfs/ucfgusfs -l usfs0 >/dev/null 2>&1 || idempotent_failure=1
/usr/lib/methods/usfs/ucfgusfs -l usfs0 >/dev/null 2>&1 || idempotent_failure=1
is_defined || idempotent_failure=1
restore_available || idempotent_failure=1
tap_ok "$idempotent_failure" "direct configure and unconfigure methods are idempotent"

rollback_failure=0
rmdev -l usfs0 >/dev/null 2>&1 || rollback_failure=1
USFS_TEST_INIT_FAULT=5 USFS_TEST_INIT_ERRNO=16 USFS_TEST_METHOD_FAULT=8 \
    mkdev -l usfs0 >/tmp/usfs-method-config.log 2>&1 && rollback_failure=1
[ -c /dev/usfs0 ] || rollback_failure=1
owned_number=$(ls -ln /dev/usfs0 2>/dev/null | awk '{ print $5 $6 }')
[ -n "$owned_number" ] || rollback_failure=1
# A retry cannot replace a registration whose rollback has not completed.
/usr/lib/methods/usfs/cfgusfs -l usfs0 >/dev/null 2>&1 && rollback_failure=1
[ "$(ls -ln /dev/usfs0 2>/dev/null | awk '{ print $5 $6 }')" = "$owned_number" ] || rollback_failure=1
/usr/lib/methods/usfs/ucfgusfs -l usfs0 >/dev/null 2>&1 || rollback_failure=1
is_defined || rollback_failure=1
restore_available || rollback_failure=1
is_available || rollback_failure=1
tap_ok "$rollback_failure" "failed CFG_TERM after ODM failure retains its original number until cleanup succeeds"

query_failure=0
owned_number=$(ls -ln /dev/usfs0 | awk '{ print $5 $6 }')
USFS_TEST_METHOD_FAULT=9 rmdev -l usfs0 \
    >/tmp/usfs-method-unconfig.log 2>&1 && query_failure=1
is_available || query_failure=1
[ "$(ls -ln /dev/usfs0 | awk '{ print $5 $6 }')" = "$owned_number" ] || query_failure=1
rmdev -l usfs0 >/dev/null 2>&1 || query_failure=1
is_defined || query_failure=1
restore_available || query_failure=1
tap_ok "$query_failure" "lifecycle query errors preserve live registration ownership and allow later cleanup"

registry_failure=0
registry_base=/tmp/usfs-vfs-reconcile.$$
mkdir "$registry_base" || exit 1
cp -p /etc/vfs "$registry_base/original" || exit 1
# Only this test-owned disposable VM registry is changed. Preserve a complete
# original and restore it on interruption as well as normal completion.
restore_registry()
{
    if cp -p "$registry_base/original" /etc/vfs && cmp "$registry_base/original" /etc/vfs; then
        rm -rf "$registry_base"
    else
        tap_diag "VFS restoration failed; original retained at $registry_base/original"
        return 1
    fi
}
trap restore_registry 0 1 2 15
expected_type=$(awk '$1 == "usfs" { print $2 }' "$registry_base/original")
awk '$1 != "usfs" { print }' "$registry_base/original" >"$registry_base/unrelated"
printf '# USFS registry preservation fixture\n' >>"$registry_base/unrelated"
cat "$registry_base/unrelated" >/etc/vfs
printf 'usfs 999 none none\nusfs 998 wrong wrong\n' >>/etc/vfs
/usr/lib/methods/usfs/cfgusfs -l usfs0 >/dev/null 2>&1 || registry_failure=1
[ "$(vfs_count)" -eq 1 ] || registry_failure=1
awk -v expected="$expected_type" '$1 == "usfs" { if (NF != 4 || $2 != expected || $3 != "none" || $4 != "none") bad=1 } END { exit bad }' /etc/vfs || registry_failure=1
awk '$1 != "usfs" { print }' /etc/vfs >"$registry_base/after"
cmp "$registry_base/unrelated" "$registry_base/after" || registry_failure=1
if restore_registry; then
    trap - 0 1 2 15
else
    registry_failure=1
fi
is_available || registry_failure=1
tap_ok "$registry_failure" "stale and duplicate VFS entries reconcile to the live type while unrelated entries survive"

rm -f /tmp/usfs-method-config.log /tmp/usfs-method-unconfig.log
tap_finish

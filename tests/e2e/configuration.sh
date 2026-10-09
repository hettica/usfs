#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   End-to-end configuration control-plane serialization and lifecycle journey.
#
# Usage:
#   Run through `make test-e2e` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/e2e/configuration.sh

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

tap_plan 7

first_marker="/tmp/usfs-config-first.$$"
second_marker="/tmp/usfs-config-second.$$"
first_log="/tmp/usfs-config-first.$$.log"
second_log="/tmp/usfs-config-second.$$.log"
rm -f "$first_marker" "$second_marker" "$first_log" "$second_log"

USFS_TEST_METHOD_LOCK_MARKER="$first_marker" \
USFS_TEST_METHOD_HOLD_MS=2500 \
    /usr/lib/methods/usfs/cfgusfs -l usfs0 >"$first_log" 2>&1 &
first_pid=$!

if usfs_wait_until "first configuration lock acquisition" 10 test -f "$first_marker"; then
    tap_ok 0 "configure method acquired the transaction lock"
else
    tap_ok 1 "configure method acquired the transaction lock"
fi

USFS_TEST_METHOD_LOCK_MARKER="$second_marker" \
    /usr/lib/methods/usfs/chusfs -l usfs0 >"$second_log" 2>&1 &
second_pid=$!
sleep 1
if [ ! -e "$second_marker" ] && kill -0 "$second_pid" 2>/dev/null; then
    tap_ok 0 "chusfs waits behind an active configure transaction"
else
    tap_ok 1 "chusfs waits behind an active configure transaction"
fi

first_rc=0
second_rc=0
wait "$first_pid" || first_rc=$?
wait "$second_pid" || second_rc=$?
if [ "$first_rc" -eq 0 ] && [ "$second_rc" -eq 0 ] &&
   [ -e "$second_marker" ]; then
    tap_ok 0 "serialized configuration commands both complete"
else
    sed 's/^/# /' "$first_log" 2>/dev/null
    sed 's/^/# /' "$second_log" 2>/dev/null
    tap_ok 1 "serialized configuration commands both complete"
fi

lifecycle_failure=0
usfs_coverage_checkpoint configuration-pre-term || lifecycle_failure=1
usfs_coverage_expect_export configuration-term || lifecycle_failure=1
USFS_COVERAGE_EVENT=configuration-term \
    rmdev -l usfs0 >/dev/null 2>&1 || lifecycle_failure=1
/usr/lib/methods/usfs/ucfgusfs -l usfs0 >/dev/null 2>&1 || lifecycle_failure=1
[ "$(lsdev -l usfs0 -F status 2>/dev/null)" = "Defined" ] || lifecycle_failure=1
[ ! -e /dev/usfs0 ] || lifecycle_failure=1
mkdev -l usfs0 >/dev/null 2>&1 || lifecycle_failure=1
mkdev -l usfs0 >/dev/null 2>&1 || lifecycle_failure=1
[ "$(lsdev -l usfs0 -F status 2>/dev/null)" = "Available" ] || lifecycle_failure=1
[ -c /dev/usfs0 ] || lifecycle_failure=1
[ "$(awk '$1 == "usfs" { count += 1 } END { print count + 0 }' /etc/vfs)" -eq 1 ] || lifecycle_failure=1
/usr/lib/methods/usfs/chusfs -l usfs0 >/dev/null 2>&1 || lifecycle_failure=1
tap_ok "$lifecycle_failure" "repeated teardown and configure converge to one usable device"

install_failure=0
install_variant=testing
[ ! -x /usr/sbin/usfs_coverage ] || install_variant=coverage
usfs_coverage_checkpoint configuration-pre-install || install_failure=1
usfs_coverage_expect_export configuration-install || install_failure=1
if USFS_COVERAGE_EVENT=configuration-install rmdev -l usfs0 >/dev/null 2>&1; then
    # A fresh copy of these tracked shell files has mode 0644. Recreate that
    # condition even when an RPM has already populated the methods directory.
    chmod 644 /usr/lib/methods/usfs/package_lifecycle \
        /usr/lib/methods/usfs/boot_rule || install_failure=1
    bash "$USFS_SOURCE_DIR/scripts/aix/install-variant.sh" "$install_variant" \
        "$USFS_SOURCE_DIR" "${USFS_BINARY_DIR:?}" \
        >"$first_log" 2>&1 || install_failure=1
    [ -x /usr/lib/methods/usfs/package_lifecycle ] || install_failure=1
    [ -x /usr/lib/methods/usfs/boot_rule ] || install_failure=1
    # Recovery must remain possible when running this regression before the fix.
    chmod 550 /usr/lib/methods/usfs/package_lifecycle \
        /usr/lib/methods/usfs/boot_rule || install_failure=1
    mkdev -l usfs0 >/dev/null 2>&1 || install_failure=1
else
    install_failure=1
fi
[ "$install_failure" -eq 0 ] || sed 's/^/# /' "$first_log" 2>/dev/null
tap_ok "$install_failure" "variant installation prepares executable methods before use"

owner_failure=0
usfs_mount_owner_case ordinary 1 || owner_failure=1
tap_ok "$owner_failure" "old client cleanup leaves a replacement mount at the same path usable"
owner_failure=0
usfs_mount_owner_case ordinary 2 || owner_failure=1
tap_ok "$owner_failure" "old client cleanup leaves both stacked replacement mounts untouched"

rm -f "$first_marker" "$second_marker" "$first_log" "$second_log"
tap_finish

#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify the configured device policy and privileged control-plane ioctls.
#
# Usage:
#   On a testing AIX deployment: bash tests/integration/aix/contract/control_plane_security.sh

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-security.$$
NODE=/dev/usfs0
SCENARIO_DAEMON=${USFS_SCENARIO_DAEMON:-/usr/sbin/usfs_scenario_daemon}

restore_mode()
{
    chmod 660 "$NODE" 2>/dev/null
    chown root:system "$NODE" 2>/dev/null
    rm -rf "$BASE"
}

trap restore_mode 0 1 2 15
mkdir -p "$BASE"
tap_plan 9

set -- $(ls -l "$NODE" 2>/dev/null)
tap_ok $( [ "${1:-}" = crw-rw---- ] && [ "${3:-}" = root ] &&
          [ "${4:-}" = system ]; echo $? ) \
    "control device is explicitly root:system 0660"

if su nobody -c "/usr/sbin/usfs_probe" >"$BASE/probe.log" 2>&1; then
    tap_ok 1 "unprivileged user cannot allocate a daemon channel"
else
    tap_ok 0 "unprivileged user cannot allocate a daemon channel"
fi

if su nobody -c "/usr/sbin/usfsctl status" >"$BASE/status.log" 2>&1; then
    tap_ok 1 "unprivileged user cannot open the status channel"
else
    tap_ok 0 "unprivileged user cannot open the status channel"
fi

if /usr/sbin/usfs_probe >"$BASE/root-probe.log" 2>&1; then
    tap_ok 0 "privileged daemon handshake remains available"
else
    tap_ok 1 "privileged daemon handshake remains available"
fi

# Temporarily make the node openable to prove the kernel authorization check
# is independent of filesystem mode.  The trap restores the production mode.
chmod 666 "$NODE"
if su nobody -c "/usr/sbin/chusfs -l usfs0 -a request_timeout_ms=30100" \
   >"$BASE/change.out" 2>&1; then
    tap_ok 1 "unprivileged caller cannot change runtime policy"
else
    tap_ok 0 "unprivileged caller cannot change runtime policy"
fi

# Both owned and root-owned mountpoints must require mount authority even
# after device access and a valid per-connection cookie have been granted.
mkdir -p "$BASE/caller-mount" "$BASE/root-mount" "$BASE/caller-logs"
chmod 755 "$BASE" "$BASE/root-mount"
chown nobody "$BASE/caller-mount" "$BASE/caller-logs"
printf 'USFS-SCENARIO 1\n' >"$BASE/scenario"
for stub in caller-mount root-mount; do
    authority_failure=0
    su nobody -c "USFS_SCENARIO_AUTHORITY_PROBE=1 '$SCENARIO_DAEMON' '$BASE/$stub' '$BASE/scenario' '$BASE/caller-logs/$stub.requests'" \
        >"$BASE/$stub.log" 2>&1 || authority_failure=1
    grep -q CHANNEL_ADMITTED "$BASE/$stub.log" || authority_failure=1
    grep -q MOUNT_AUTHORITY_DENIED "$BASE/$stub.log" || authority_failure=1
    if mount | grep -q " $BASE/$stub "; then
        authority_failure=1
        umount -f "$BASE/$stub" || authority_failure=1
    fi
    tap_ok "$authority_failure" "device-admitted caller cannot mount at $stub"
done

chmod 660 "$NODE"
chown root:system "$NODE"
set -- $(ls -l "$NODE")
tap_ok $( [ "$1" = crw-rw---- ] && [ "$3" = root ] &&
          [ "$4" = system ]; echo $? ) \
    "security test restores the production node policy"

if /usr/sbin/chusfs -l usfs0 -a request_timeout_ms=30000 \
   >"$BASE/root-change.out" 2>&1; then
    tap_ok 0 "privileged runtime administration remains available"
else
    tap_ok 1 "privileged runtime administration remains available"
fi

tap_finish

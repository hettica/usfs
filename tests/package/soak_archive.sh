#!/usr/bin/env bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Build and qualify the production-RPM real-AIX soak archive, including its
#   inventory, checksums, bounded smoke, failure capture, and isolated journeys.
#
# Usage:
#   From an AIX checkout as root:
#     bash tests/package/soak_archive.sh

set -u
AIX_VERSION=$(uname -v).$(uname -r)
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
BUILD_ROOT=${USFS_SOAK_TEST_BUILD:-$ROOT/build/soak-qualification}
# Campaign data needs substantially more space than the small AIX /var
# filesystem commonly provides. /home is also the documented operator
# workspace and satisfies the bounded automation reserve in the VM image.
WORK_ROOT=${USFS_SOAK_TEST_WORKDIR:-/home/usfs-soak-runs}
REVISION=soak-test
NAME="usfs-soak-0.1.0-$REVISION-aix$AIX_VERSION-ppc64"
LOCAL_ARCHIVE="$BUILD_ROOT/packages/$NAME.tar.gz"
FAILED=0
NUMBER=0

report()
{
    description=$1
    shift
    NUMBER=$((NUMBER + 1))
    if "$@"; then
        echo "ok $NUMBER - $description"
    else
        echo "not ok $NUMBER - $description"
        FAILED=1
    fi
}

remote()
{
    bash -c "$1"
}

remote_with_heartbeat()
{
    bash -c "$1" &
    remote_pid=$!
    elapsed=0
    while kill -0 "$remote_pid" 2>/dev/null; do
        sleep 30
        if kill -0 "$remote_pid" 2>/dev/null; then
            elapsed=$((elapsed + 30))
            echo "# bounded soak smoke running: ${elapsed}s"
        fi
    done
    wait "$remote_pid"
}

restore()
{
    bash "$ROOT/scripts/aix/cleanup.sh" >/dev/null 2>&1 || true
    if lsdev -l usfs0 2>/dev/null | grep -q '^usfs0'; then
        rmdev -l usfs0 >/dev/null 2>&1 || true
    fi
    bash "$ROOT/scripts/aix/install-variant.sh" release "$ROOT" "$BUILD_ROOT" \
        >/dev/null 2>&1 || true
    bash "$ROOT/scripts/aix/setup.sh" >/dev/null 2>&1 || true
}
trap restore EXIT HUP INT TERM

echo "TAP version 13"
echo "1..8"

rm -rf "$BUILD_ROOT"
cmake -S "$ROOT" -B "$BUILD_ROOT" \
    -DUSFS_BUILD_REVISION="$REVISION" -DUSFS_BUILD_DIRTY=no >/dev/null || exit 1
cmake --build "$BUILD_ROOT" --target soak >/dev/null || exit 1
restore

report "archive has the exact release inventory" bash -c '
  entries=$(gzip -dc "$1" | tar -tf -)
  for path in script.sh lifecycle.sh nested-mount.sh README.md manifest.txt CHECKSUMS.sha256 \
    bin/usfs_mirror bin/usfs_memfs bin/usfs_scenario_daemon \
    bin/usfs_io_probe bin/usfsctl bin/usfs_soak_worker \
    bin/usfs_admin_probe \
    conf/defaults.conf lib/common.ksh; do
      grep -qx "$2/$path" <<<"$entries" || exit 1
  done
' _ "$LOCAL_ARCHIVE" "$NAME"

report "archive excludes testing kernel and private control tools" bash -c '
  ! gzip -dc "$1" | tar -tf - | grep -Eq "usfs[.]kext|usfs_testctl|usfs_usercopy_test|usfs_coverage"
' _ "$LOCAL_ARCHIVE"

report "archive paths are relative and cannot escape extraction" bash -c '
  ! gzip -dc "$1" | tar -tf - | grep -Eq "^/|(^|/)[.][.](/|$)"
' _ "$LOCAL_ARCHIVE"

report "every archive payload checksum verifies" remote "set -e; d=/var/tmp/$NAME.check; rm -rf \$d; mkdir \$d; gzip -dc $LOCAL_ARCHIVE | (cd \$d && tar -xf -); cd \$d/$NAME; while read sum file; do file=\$(echo \$file | sed 's/^  *//'); actual=\$(perl bin/sha256.pl \$file | awk '{print \$1}'); test \"\$actual\" = \"\$sum\"; done <CHECKSUMS.sha256"

report "production RPM installs and matches archive manifest" remote_with_heartbeat "rpm -Uvh --replacepkgs $BUILD_ROOT/packages/usfs-0.1.0-1.aix$AIX_VERSION.ppc.rpm >/var/tmp/usfs-soak-rpm.log 2>&1 && rpm -V usfs && test \"\$(rpm -q usfs)\" = usfs-0.1.0-1.ppc"

SMOKE_SECONDS=${SOAK_SMOKE_SECONDS:-600}
SMOKE_RESTART_SECONDS=${SOAK_SMOKE_RESTART_SECONDS:-120}
report "bounded campaign survives coordinated volatile-memfs restarts" remote_with_heartbeat "cd /var/tmp/$NAME.check/$NAME && USFS_SOAK_SECONDS=$SMOKE_SECONDS USFS_SOAK_RESTART_SECONDS=$SMOKE_RESTART_SECONDS USFS_SOAK_WORKERS=2 USFS_SOAK_TEST_UNDERSIZED=1 USFS_SOAK_WORKDIR=$WORK_ROOT ./script.sh 1 && latest=\$(ls -td $WORK_ROOT/run-* | sed -n '1p') && grep -q 'DAEMON_RESTART id=memfs1 style=graceful' \$latest/events.log && grep -q 'DAEMON_RESTART id=memfs2 style=abrupt' \$latest/events.log && grep -q '^state=completed$' \$latest/state/workers/worker1.status && grep -q '^state=completed$' \$latest/state/workers/worker2.status"

report "injected worker failure captures diagnostics and ordinary cleanup" remote "cd /var/tmp/$NAME.check/$NAME && ! USFS_SOAK_SECONDS=60 USFS_SOAK_WORKERS=2 USFS_SOAK_TEST_UNDERSIZED=1 USFS_SOAK_WORKDIR=$WORK_ROOT USFS_SOAK_INJECT_WORKER_FAILURE=2 ./script.sh 1 >/var/tmp/usfs-soak-injected.log 2>&1 && latest=\$(ls -td $WORK_ROOT/run-* | sed -n '1p') && test -s \$latest/failure-summary.txt && test -d \$latest/diagnostics && test -s \$latest/diagnostics/uname.txt && test -s \$latest/diagnostics/usfs.trc && test -s \$latest/diagnostics/usfs.trc.txt && ! mount | grep -F \"\$latest\""

report "nested and isolated lifecycle journeys pass" remote "cd /var/tmp/$NAME.check/$NAME && ./nested-mount.sh 1 && ./lifecycle.sh 1"

restore
trap - EXIT HUP INT TERM
[ "$FAILED" -eq 0 ]

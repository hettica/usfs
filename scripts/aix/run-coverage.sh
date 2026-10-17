#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Exercise instrumented AIX workloads and render optional HTML reports.
# Usage: bash scripts/aix/run-coverage.sh source build [run] [revision] [dirty]

set -eu

# This optional reporting command remains noninteractive. Workload and cleanup
# errors retain their status; no coverage metric decides success or release readiness.
exec </dev/null

USFS_SOURCE_DIR=$1
USFS_BINARY_DIR=$2
requested_run=${3:-quick}
revision=${4:-unknown}
dirty=${5:-unknown}
export USFS_SOURCE_DIR USFS_BINARY_DIR

for tool in gcov lcov genhtml bash perl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "coverage requires '$tool' on the AIX host" >&2
        exit 1
    }
done
gcov --version | sed -n '1p' | grep -q '13.3.0' || {
    echo "coverage requires gcov 13.3.0 on the AIX host" >&2
    exit 1
}
lcov --version

if [ "$requested_run" = quick ]; then
    run="coverage-$(TZ=UTC date '+%Y%m%dT%H%M%SZ')"
else
    run=$requested_run
fi
case "$run" in
    ""|*[!A-Za-z0-9._-]*) echo "invalid coverage run name: $run" >&2; exit 2 ;;
esac

active=0
collected=0
current_suite=

event()
{
    bash "$USFS_SOURCE_DIR/scripts/aix/coverage-checkpoint.sh" \
        event "$run" "$1" "$2"
}

collect_partial()
{
    [ "$active" -eq 1 ] || return 0
    [ "$collected" -eq 0 ] || return 0
    echo "Attempting partial coverage collection after failure" >&2
    bash "$USFS_SOURCE_DIR/scripts/aix/cleanup.sh" >/dev/null 2>&1 || true
    if bash "$USFS_SOURCE_DIR/scripts/aix/coverage-collect.sh" "$run" partial; then
        collected=1
        bash "$USFS_SOURCE_DIR/scripts/coverage-report.sh" "$run" || true
        echo "Partial coverage report: $USFS_BINARY_DIR/coverage/$run" >&2
    else
        echo "ERROR: partial coverage collection failed" >&2
    fi
}

on_exit()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$status" -ne 0 ]; then
        [ -z "$current_suite" ] || event "$current_suite" failed || true
        collect_partial
    fi
    exit "$status"
}
trap on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_suite()
{
    suite=$1
    shift
    current_suite=$suite
    event "$suite" started
    if [ "${USFS_COVERAGE_FAIL_SUITE:-}" = "$suite" ]; then
        echo "injected coverage workflow failure at suite: $suite" >&2
        return 1
    fi
    "$@"
    event "$suite" completed
    current_suite=
}

run_scripts()
{
    for script in "$@"; do
        echo "== $script"
        (cd "$USFS_SOURCE_DIR" && bash "$script")
    done
}

bash "$USFS_SOURCE_DIR/tests/unit/coverage_export_test.sh"

bash "$USFS_SOURCE_DIR/scripts/aix/cleanup.sh" >/dev/null 2>&1 || true
if lsdev -l usfs0 2>/dev/null | grep -q '^usfs0'; then
    rmdev -l usfs0 >/dev/null
fi
bash "$USFS_SOURCE_DIR/scripts/aix/install-variant.sh" coverage \
    "$USFS_SOURCE_DIR" "$USFS_BINARY_DIR"
bash "$USFS_SOURCE_DIR/scripts/aix/setup.sh"
active=1

mkdir -p "$USFS_BINARY_DIR/coverage"
{
    echo "git_revision=$revision"
    echo "worktree_dirty=$dirty"
    perl "$USFS_SOURCE_DIR/scripts/aix/sha256.pl" \
        "$USFS_BINARY_DIR/coverage/bin/usfs.kext" \
        "$USFS_BINARY_DIR/coverage/bin/libusfs.a" \
        "$USFS_BINARY_DIR/coverage/bin/usfs_mirror" \
        "$USFS_BINARY_DIR/coverage/bin/usfs_memfs" \
        "$USFS_BINARY_DIR/coverage/bin/cfgusfs" \
        "$USFS_BINARY_DIR/coverage/bin/ucfgusfs" \
        "$USFS_BINARY_DIR/coverage/bin/chusfs" \
        "$USFS_BINARY_DIR/coverage/bin/usfsctl"
    echo source_manifest_begin
    (cd "$USFS_SOURCE_DIR" && bash scripts/aix/source-manifest.sh)
    echo source_manifest_end
} >"$USFS_BINARY_DIR/coverage/build-manifest.txt"

bash "$USFS_SOURCE_DIR/scripts/aix/coverage-checkpoint.sh" begin "$run" full
USFS_COVERAGE_SUITE=$run
export USFS_COVERAGE_SUITE
# Discover the same live-test directories used by the ordinary test targets.
# Performance and release qualification are separate from instrumented workloads.
for suite in unit-aix kernel-unit-aix contract fault concurrency e2e; do
    event "$suite" planned
done

run_suite unit-aix bash -ec '
    for unit in "$USFS_BINARY_DIR"/coverage/aix-unit/*; do
        [ -x "$unit" ] || continue
        echo "== $(basename "$unit")"
        "$unit"
    done
'
run_suite kernel-unit-aix /usr/sbin/usfs_testctl selftest all
for group in contract fault concurrency; do
    run_suite "$group" run_scripts "$USFS_SOURCE_DIR"/tests/integration/aix/"$group"/*.sh
done
run_suite e2e run_scripts "$USFS_SOURCE_DIR"/tests/e2e/*.sh

bash "$USFS_SOURCE_DIR/scripts/aix/cleanup.sh"
bash "$USFS_SOURCE_DIR/scripts/aix/coverage-collect.sh" "$run" complete
collected=1
bash "$USFS_SOURCE_DIR/scripts/coverage-report.sh" "$run"

echo "Coverage reports: $USFS_BINARY_DIR/coverage/$run"

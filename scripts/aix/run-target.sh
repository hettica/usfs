#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose: Run a public USFS test target directly on the AIX host.
# Usage: bash scripts/aix/run-target.sh test|test-e2e source build

set -eu

# Public test targets are unattended gates. No test utility may inherit the
# terminal and wait on a hidden confirmation prompt.
exec </dev/null
unset USFS_COVERAGE_SUITE USFS_COVERAGE_EVENT

mode=$1
USFS_SOURCE_DIR=$2
USFS_BINARY_DIR=$3
export USFS_SOURCE_DIR USFS_BINARY_DIR

test "$(id -u)" -eq 0 || {
    echo "$mode requires an AIX root session to install and reload USFS" >&2
    exit 1
}

activate()
{
    variant=$1
    bash "$USFS_SOURCE_DIR/scripts/aix/cleanup.sh" >/dev/null 2>&1 || true
    if lsdev -l usfs0 2>/dev/null | grep -q '^usfs0'; then
        rmdev -l usfs0 >/dev/null
    fi
    bash "$USFS_SOURCE_DIR/scripts/aix/install-variant.sh" \
        "$variant" "$USFS_SOURCE_DIR" "$USFS_BINARY_DIR"
    bash "$USFS_SOURCE_DIR/scripts/aix/setup.sh"
}

restore_release()
{
    restore_status=$?
    trap - EXIT HUP INT TERM
    echo "Restoring ordinary USFS release build"
    if ! activate release; then
        echo "ERROR: failed to restore the ordinary release build" >&2
        exit 1
    fi
    exit "$restore_status"
}
trap restore_release EXIT HUP INT TERM

run_units()
{
    unit_dir=$1
    for unit in "$unit_dir"/*; do
        [ -x "$unit" ] || continue
        echo "== $(basename "$unit")"
        "$unit"
    done
}

run_compile_failure_test()
{
    output="$USFS_BINARY_DIR/tests/synchronized_type_mismatch_test"
    if gcc -maix64 -Wall -Wextra -Werror \
        -I"$USFS_SOURCE_DIR/src/kernel" \
        "$USFS_SOURCE_DIR/tests/unit/synchronized_type_mismatch_test.c" \
        -o "$output" >/dev/null 2>&1; then
        echo "synchronization type-mismatch test unexpectedly compiled" >&2
        return 1
    fi
    echo "ok - synchronization type mismatch is rejected at compile time"
}

run_optional_sanitizer()
{
    probe="$USFS_BINARY_DIR/tests/sanitizer-probe"
    if gcc -maix64 -fsanitize=address,undefined \
        "$USFS_SOURCE_DIR/tests/harness/tap.c" \
        "$USFS_SOURCE_DIR/src/common/usfs_validate.c" \
        "$USFS_SOURCE_DIR/src/common/usfs_status.c" \
        "$USFS_SOURCE_DIR/tests/unit/protocol_test.c" \
        -I"$USFS_SOURCE_DIR/tests/harness" -I"$USFS_SOURCE_DIR/src/common" \
        -o "$probe" >/dev/null 2>&1; then
        ASAN_OPTIONS=detect_leaks=0 "$probe"
        rm -f "$probe"
    else
        echo "ok - AIX address/undefined sanitizers unavailable # SKIP"
    fi
}

run_script_group()
{
    for script in "$@"; do
        echo "== $script"
        (cd "$USFS_SOURCE_DIR" && bash "$USFS_SOURCE_DIR/$script")
    done
}

run_non_e2e()
{
    (cd "$USFS_SOURCE_DIR" && bash tests/meta/source_validation.sh)
    (cd "$USFS_SOURCE_DIR" && bash tests/unit/package_preinstall_test.sh)
    run_units "$USFS_BINARY_DIR/tests"
    /usr/bin/ksh "$USFS_SOURCE_DIR/tests/unit/aix/soak_deadline_test.ksh"
    run_compile_failure_test
    run_optional_sanitizer
    activate testing
    /usr/sbin/usfs_testctl selftest all
    run_script_group \
        tests/integration/aix/contract/protocol.sh \
        tests/integration/aix/contract/errno_matrix.sh \
        tests/integration/aix/contract/operation_validation.sh \
        tests/integration/aix/contract/handle_lifecycle.sh \
        tests/integration/aix/contract/client_file_lifetime.sh \
        tests/integration/aix/contract/io_boundaries.sh \
        tests/integration/aix/contract/vnode_compat.sh \
        tests/integration/aix/contract/file_identifiers.sh \
        tests/integration/aix/contract/forced_unmount.sh \
        tests/integration/aix/contract/access_permissions.sh \
        tests/integration/aix/contract/control_plane_security.sh \
        tests/integration/aix/contract/system_trace.sh \
        tests/integration/aix/contract/usercopy_io.sh \
        tests/integration/aix/contract/mount_metadata.sh \
        tests/integration/aix/contract/disconnect_matrix.sh \
        tests/integration/aix/contract/mmap_pager.sh \
        tests/integration/aix/fault/configuration_methods.sh \
        tests/integration/aix/fault/init_retry.sh \
        tests/integration/aix/fault/init_matrix.sh \
        tests/integration/aix/fault/runtime.sh \
        tests/integration/aix/fault/allocation_matrix.sh \
        tests/integration/aix/fault/lifecycle_timeout.sh \
        tests/integration/aix/concurrency/controlled.sh \
        tests/integration/aix/concurrency/vnode_identity.sh \
        tests/integration/aix/concurrency/lifecycle.sh
}

run_e2e()
{
    activate testing
    run_script_group \
        tests/e2e/configuration.sh \
        tests/e2e/multithread.sh \
        tests/e2e/mirror.sh \
        tests/e2e/memfs.sh \
        tests/e2e/availability.sh \
        tests/e2e/channel_capacity.sh \
        tests/e2e/observability.sh
}

case "$mode" in
    test) run_non_e2e ;;
    test-e2e) run_e2e ;;
    *) echo "unknown public target runner mode: $mode" >&2; exit 2 ;;
esac

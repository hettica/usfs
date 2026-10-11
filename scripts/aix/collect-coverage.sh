#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Build and run non-performance AIX tests, then archive coverage evidence.
# Usage: bash scripts/aix/collect-coverage.sh RUN-DIRECTORY RUN REVISION DIRTY

set -euo pipefail
# Give every logged step its own process group for interruption cleanup.
set -m
exec </dev/null
export PATH=/opt/freeware/bin:/usr/bin:/usr/sbin:/etc:$PATH

readonly heartbeat_seconds=30
readonly cancellation_grace_seconds=30
readonly checksum_progress_files=100
active_command_pid=
active_step=
readonly run_directory=$1
readonly run=$2
readonly revision=$3
readonly dirty=$4
readonly source_directory="$run_directory/source"
readonly build_directory="$run_directory/build"
readonly bundle_directory="$run_directory/usfs-$run"
readonly archive_path="$run_directory/usfs-$run.tar.gz"
readonly log_directory="$bundle_directory/logs"

export USFS_SOURCE_DIR="$source_directory"
export USFS_BINARY_DIR="$build_directory"
# Keep compiler scratch files on the operator-selected workspace filesystem.
export TMPDIR="$run_directory/tmp"
mkdir -p "$log_directory" "$TMPDIR"
printf 'step\texit_status\n' >"$bundle_directory/test-results.tsv"

run_logged()
{
    local step=$1
    shift
    echo "Starting $step"
    "$@" >"$log_directory/$step.log" 2>&1 &
    local command_pid=$!
    active_command_pid=$command_pid
    active_step=$step
    while kill -0 "$command_pid" 2>/dev/null; do
        sleep "$heartbeat_seconds"
        if kill -0 "$command_pid" 2>/dev/null; then
            echo "Running $step ($(TZ=UTC date '+%Y-%m-%dT%H:%M:%SZ'))"
            tail -n 3 "$log_directory/$step.log"
        fi
    done
    local rc=0
    wait "$command_pid" || rc=$?
    active_command_pid=
    active_step=
    printf '%s\t%s\n' "$step" "$rc" >>"$bundle_directory/test-results.tsv"
    tail -n 12 "$log_directory/$step.log"
    if [ "$rc" -ne 0 ]; then
        echo "Failed AIX step $step: status=$rc; see $log_directory/$step.log" >&2
    fi
    return "$rc"
}

stop_active_command()
{
    [ -n "$active_command_pid" ] || return 0
    echo "Stopping interrupted AIX step: $active_step"
    kill -TERM -- "-$active_command_pid" 2>/dev/null || true
    local deadline=$((SECONDS + cancellation_grace_seconds))
    while kill -0 -- "-$active_command_pid" 2>/dev/null; do
        if [ "$SECONDS" -ge "$deadline" ]; then
            kill -KILL -- "-$active_command_pid" 2>/dev/null || true
            break
        fi
        sleep 1
    done
    wait "$active_command_pid" 2>/dev/null || true
}

release_artifacts_ready()
{
    for artifact in usfs.kext cfgusfs ucfgusfs chusfs usfs_mirror usfs_memfs usfs_probe usfsctl; do
        [ -s "$build_directory/bin/$artifact" ] || return 1
    done
    return 0
}

archive_results()
{
    local original_status=$?
    trap - EXIT HUP INT TERM
    local archive_status=0
    stop_active_command
    if [ -n "$active_step" ]; then
        printf '%s\t%s\n' "$active_step" "$original_status" >>"$bundle_directory/test-results.tsv"
    fi
    # Always restore this run's release, including package/soak failure paths.
    # Avoid installed-state changes if configuration failed before a build.
    if release_artifacts_ready; then
        local restore_status=0
        bash "$source_directory/scripts/aix/restore-release.sh" \
            "$source_directory" "$build_directory" >"$log_directory/final-restoration.log" 2>&1 || restore_status=$?
        printf 'final-restoration\t%s\n' "$restore_status" >>"$bundle_directory/test-results.tsv"
        if [ "$restore_status" -ne 0 ]; then
            echo "Failed to restore ordinary release: see $log_directory/final-restoration.log" >&2
            original_status=1
        fi
    fi
    printf 'workflow_exit_status=%s\n' "$original_status" >"$bundle_directory/status.txt"
    printf 'revision=%s\nworktree_dirty=%s\n' "$revision" "$dirty" >>"$bundle_directory/status.txt"
    printf 'remote_source=%s\n' "$source_directory" >>"$bundle_directory/status.txt"
    cp "$run_directory/source.tar.gz" "$bundle_directory/source.tar.gz" || archive_status=1

    if [ -f "$build_directory/coverage/$run/manifest.txt" ]; then
        # Both directories belong to this fresh run on the same filesystem.
        # Move the report instead of duplicating thousands of profile files.
        mv "$build_directory/coverage/$run" "$bundle_directory/coverage" || archive_status=1
    fi
    if [ -f "$bundle_directory/coverage/all.info" ]; then
        perl "$source_directory/scripts/aix/export-coverage.pl" \
            "$source_directory" "$bundle_directory/coverage/all.info" \
            "$bundle_directory/coverage.lcov" "$bundle_directory/sonarqube.xml" || archive_status=1
    fi
    cp "$source_directory/docs/coverage-archive.md" "$bundle_directory/README.md" || archive_status=1
    (
        cd "$bundle_directory"
        echo "Checksumming coverage archive payload"
        count=0
        find . -type f ! -name CHECKSUMS.sha256 -print | LC_ALL=C sort |
            while IFS= read -r path; do
                perl "$source_directory/scripts/aix/sha256.pl" "$path" || exit 1
                count=$((count + 1))
                if [ "$((count % checksum_progress_files))" -eq 0 ]; then
                    echo "Checksummed $count archive files" >&2
                fi
            done >CHECKSUMS.sha256
    ) || archive_status=1
    if [ "$archive_status" -eq 0 ]; then
        (cd "$run_directory" && tar -cf - "usfs-$run" | gzip -c >"$archive_path") || archive_status=1
    fi
    if [ "$archive_status" -ne 0 ]; then
        echo "Failed to archive coverage evidence: inspect $bundle_directory" >&2
        exit 1
    fi
    echo "Created coverage evidence archive: $archive_path"
    exit "$original_status"
}

trap archive_results EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_logged configure cmake -S "$source_directory" -B "$build_directory" \
    -DUSFS_COVERAGE_RUN="$run" -DUSFS_BUILD_REVISION="$revision" -DUSFS_BUILD_DIRTY="$dirty"

cd "$build_directory"
# The ordinary gates execute source checks, shell/compile-failure checks and
# method tests that deliberately require the uninstrumented testing variant.
run_logged test make test
run_logged test-e2e make test-e2e
run_logged coverage make coverage

# These journeys test production installation and archive behavior, so their
# release binaries are intentionally separate from instrumented measurements.
# Both restore the ordinary release installation before returning.
USFS_PACKAGE_TEST_BUILD="$build_directory/package-lifecycle"
export USFS_PACKAGE_TEST_BUILD
run_logged rpm-lifecycle bash "$source_directory/tests/package/rpm_lifecycle.sh"
USFS_SOAK_TEST_WORKDIR="$run_directory/soak-runs"
USFS_SOAK_TEST_BUILD="$build_directory/soak-qualification"
export USFS_SOAK_TEST_WORKDIR USFS_SOAK_TEST_BUILD
run_logged soak-archive bash "$source_directory/tests/package/soak_archive.sh"


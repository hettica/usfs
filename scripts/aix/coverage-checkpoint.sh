#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Manage one AIX kernel-coverage run: initialize its state, capture and reset
#   counter deltas, and record suite, lifecycle-export, and process-profile
#   expectations for final validation.
#
# Usage:
#   Run from the repository root on the AIX host. For example:
#     bash scripts/aix/coverage-checkpoint.sh begin nightly full
#     bash scripts/aix/coverage-checkpoint.sh snapshot nightly after-contract
#     bash scripts/aix/coverage-checkpoint.sh event nightly e2e-mirror completed
#     bash scripts/aix/coverage-checkpoint.sh expect-export nightly init-retry
#     bash scripts/aix/coverage-checkpoint.sh expect-profile nightly mirror 12345
#
#   Run without a recognized command to print the complete command synopsis.

set -eu

command=${1:-}
run=${2:-}
label=${3:-}
: "${USFS_BINARY_DIR:?USFS_BINARY_DIR is required}"
coverage_root="$USFS_BINARY_DIR/coverage"
root="$coverage_root/kernel-data"

valid_name()
{
    case "$1" in
        ""|*[!A-Za-z0-9._-]*) return 1 ;;
        *) return 0 ;;
    esac
}

valid_name "$run" || {
    echo "invalid coverage run name: $run" >&2
    exit 2
}

run_root="$root/$run"

case "$command" in
    begin)
        valid_name "$label" || {
            echo "invalid coverage profile name: $label" >&2
            exit 2
        }
        rm -rf "$run_root"
        rm -rf "$coverage_root/user-data/$run"
        mkdir -p "$run_root"
        mkdir -p "$run_root/lifecycle"
        mkdir -p "$coverage_root/user-data/$run"
        mkdir -p "$coverage_root/user-data/$run/processes"
        printf '1\n' >"$run_root/next"
        printf '2\n' >"$run_root/next-generation"
        printf '1\n' >"$run_root/current-generation"
        printf 'profile=%s\n' "$label" >"$run_root/context.txt"
        printf 'profile\t%s\n' "$label" \
            >"$coverage_root/user-data/$run/ledger.tsv"
        : >"$run_root/checkpoints.tsv"
        ;;
    snapshot)
        valid_name "$label" || {
            echo "invalid coverage checkpoint label: $label" >&2
            exit 2
        }
        [ -x /usr/sbin/usfs_coverage ] || {
            echo "coverage checkpoint requested without coverage build" >&2
            exit 1
        }
        [ -f "$run_root/next" ] || {
            echo "coverage run was not initialized: $run" >&2
            exit 1
        }
        if ! mkdir "$run_root/.lock" 2>/dev/null; then
            echo "another coverage checkpoint is active" >&2
            exit 1
        fi
        trap 'rmdir "$run_root/.lock" 2>/dev/null || true' 0 1 2 15
        sequence=$(cat "$run_root/next")
        case "$sequence" in
            ""|*[!0-9]*)
                echo "invalid coverage checkpoint sequence" >&2
                exit 1
                ;;
        esac
        [ "$sequence" -gt 0 ] && [ "$sequence" -le 9999 ] || {
            echo "coverage checkpoint sequence exhausted" >&2
            exit 1
        }
        identifier=$(printf '%04d-%s' "$sequence" "$label")
        /usr/sbin/usfs_coverage snapshot "$run_root/$identifier"
        if ! /usr/sbin/usfs_coverage reset >/dev/null; then
            rm -rf "$run_root/$identifier"
            echo "unable to reset counters after checkpoint" >&2
            exit 1
        fi
        generation=$(cat "$run_root/current-generation")
        case "$generation" in
            ""|*[!0-9]*)
                rm -rf "$run_root/$identifier"
                echo "invalid coverage generation" >&2
                exit 1
                ;;
        esac
        printf '%s\t%s\t%s\tcheckpoint\n' \
            "$identifier" "$label" "$generation" \
            >>"$run_root/checkpoints.tsv"
        printf '%s\n' $((sequence + 1)) >"$run_root/next"
        echo "kernel coverage checkpoint: $identifier"
        ;;
    event)
        event_type=${4:-}
        valid_name "$event_type" || {
            echo "invalid coverage event type: $event_type" >&2
            exit 2
        }
        valid_name "$label" || {
            echo "invalid coverage event label: $label" >&2
            exit 2
        }
        [ -f "$run_root/next" ] || {
            echo "coverage run was not initialized: $run" >&2
            exit 1
        }
        printf '%s\t%s\n' "$event_type" "$label" \
            >>"$coverage_root/user-data/$run/ledger.tsv"
        ;;
    expect-export)
        valid_name "$label" || {
            echo "invalid coverage export label: $label" >&2
            exit 2
        }
        printf 'expect-export\t%s\n' "$label" \
            >>"$coverage_root/user-data/$run/ledger.tsv"
        ;;
    expect-profile)
        pid=${4:-}
        case "$pid" in
            ""|*[!0-9]*) echo "invalid coverage process id: $pid" >&2; exit 2 ;;
        esac
        valid_name "$label" || {
            echo "invalid coverage workload: $label" >&2
            exit 2
        }
        printf 'expect-profile\t%s\t%s\n' "$pid" "$label" \
            >>"$coverage_root/user-data/$run/ledger.tsv"
        ;;
    *)
        echo "usage: coverage-checkpoint.sh begin RUN PROFILE | snapshot RUN LABEL | event RUN LABEL TYPE | expect-export RUN LABEL | expect-profile RUN WORKLOAD PID" >&2
        exit 2
        ;;
esac

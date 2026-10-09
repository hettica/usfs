#!/usr/bin/ksh
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Shared bounded lifecycle helpers for the archive-local reliability scripts.

SOAK_STATUS_DIRECTORY_ATTEMPTS=10
SOAK_COMMAND_TERM_WAIT_SECONDS=5
SOAK_COMMAND_TIMEOUT_RC=124
SOAK_COMMAND_INTERNAL_ERROR_RC=125
SOAK_MAX_EXIT_STATUS=255

soak_root_dir()
{
    (cd "$(dirname "$0")" && pwd)
}

soak_is_uint()
{
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
    esac
    return 0
}

soak_clamp()
{
    _value=$1
    _minimum=$2
    _maximum=$3
    [ "$_value" -lt "$_minimum" ] && _value=$_minimum
    [ "$_value" -gt "$_maximum" ] && _value=$_maximum
    echo "$_value"
}

soak_is_mounted()
{
    mount 2>/dev/null | awk '$3 == "usfs" { print $2 }' | grep -Fxq "$1"
}

soak_wait_mounted()
{
    _mount=$1
    _deadline=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$_deadline" ]; do
        soak_is_mounted "$_mount" && return 0
        sleep 1
    done
    return 1
}

soak_wait_pid_gone()
{
    _pid=$1
    _deadline=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$_deadline" ]; do
        kill -0 "$_pid" 2>/dev/null || return 0
        sleep 1
    done
    return 1
}

soak_bounded_kill()
{
    _pid=$1
    _signal=${2:-TERM}
    kill -"$_signal" "$_pid" 2>/dev/null || return 0
    soak_wait_pid_gone "$_pid" 30
}

soak_command_descendants()
{
    ps -e -o pid= -o ppid= | awk -v parent="$1" '
        { process_id[NR] = $1; parent_id[NR] = $2 }
        END {
            owned[parent] = 1
            for (pass = 0; pass < NR; pass++) {
                added = 0
                for (index = 1; index <= NR; index++) {
                    if (owned[parent_id[index]] && !owned[process_id[index]]) {
                        owned[process_id[index]] = 1
                        added = 1
                    }
                }
                if (!added) break
            }
            for (index = 1; index <= NR; index++)
                if (process_id[index] != parent && owned[process_id[index]])
                    print process_id[index]
        }
    '
}

soak_run_with_deadline()
{
    typeset seconds=$1
    shift
    typeset status_directory=""
    typeset attempt=0
    typeset status_directory_created=0

    while [ "$attempt" -lt "$SOAK_STATUS_DIRECTORY_ATTEMPTS" ]; do
        status_directory="${TMPDIR:-/tmp}/usfs-soak-command.$$.$RANDOM"
        if (umask 077; mkdir "$status_directory") 2>/dev/null; then
            status_directory_created=1
            break
        fi
        attempt=$((attempt + 1))
    done

    if [ "$status_directory_created" -ne 1 ]; then
        echo "Failed to create a private soak command status directory" >&2
        return "$SOAK_COMMAND_INTERNAL_ERROR_RC"
    fi

    (
        "$@"
        command_rc=$?
        printf '%s\n' "$command_rc" >"$status_directory/exit.tmp" || exit "$SOAK_COMMAND_INTERNAL_ERROR_RC"
        mv "$status_directory/exit.tmp" "$status_directory/exit" || exit "$SOAK_COMMAND_INTERNAL_ERROR_RC"
    ) &
    typeset command_pid=$!
    typeset deadline=$(( $(date +%s) + seconds ))

    while kill -0 "$command_pid" 2>/dev/null; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            typeset descendants=$(soak_command_descendants "$command_pid")
            typeset process_id

            for process_id in $descendants; do
                kill -TERM "$process_id" 2>/dev/null
            done
            kill -TERM "$command_pid" 2>/dev/null

            soak_wait_pid_gone "$command_pid" "$SOAK_COMMAND_TERM_WAIT_SECONDS" ||
                kill -KILL "$command_pid" 2>/dev/null

            for process_id in $descendants; do
                kill -0 "$process_id" 2>/dev/null && kill -KILL "$process_id" 2>/dev/null
            done

            wait "$command_pid" 2>/dev/null
            rm -f "$status_directory/exit" "$status_directory/exit.tmp"
            rmdir "$status_directory"
            return "$SOAK_COMMAND_TIMEOUT_RC"
        fi
        sleep 1
    done

    wait "$command_pid" 2>/dev/null

    typeset command_rc=""
    if [ -f "$status_directory/exit" ]; then
        IFS= read -r command_rc <"$status_directory/exit"
    fi

    rm -f "$status_directory/exit" "$status_directory/exit.tmp"
    rmdir "$status_directory"

    case "$command_rc" in
        ''|*[!0-9]*)
            echo "Failed to read the soak command exit status" >&2
            return "$SOAK_COMMAND_INTERNAL_ERROR_RC"
            ;;
    esac

    if [ "$command_rc" -gt "$SOAK_MAX_EXIT_STATUS" ]; then
        echo "Invalid soak command exit status: $command_rc" >&2
        return "$SOAK_COMMAND_INTERNAL_ERROR_RC"
    fi

    return "$command_rc"
}

soak_capture_command()
{
    _output=$1
    shift
    {
        echo "command: $*"
        "$@"
        echo "exit: $?"
    } >"$_output" 2>&1
}

soak_ordinary_cleanup()
{
    _run=$1
    _leftovers=""

    [ -d "$_run/state/workers" ] && touch "$_run/state/stop"
    if [ -d "$_run/state/workers" ]; then
        for _worker in "$_run"/state/workers/*.pid; do
            [ -f "$_worker" ] || continue
            _pid=$(sed -n '1p' "$_worker")
            soak_wait_pid_gone "$_pid" 20 || soak_bounded_kill "$_pid" TERM
        done
    fi
    if [ -d "$_run/state/daemons" ]; then
        for _mount_file in "$_run"/state/daemons/*.mount; do
            [ -f "$_mount_file" ] || continue
            _mount=$(sed -n '1p' "$_mount_file")
            if soak_is_mounted "$_mount"; then
                soak_run_with_deadline 30 umount "$_mount" >/dev/null 2>&1 ||
                    _leftovers="$_leftovers mount:$_mount"
            fi
        done
        for _pid_file in "$_run"/state/daemons/*.pid; do
            [ -f "$_pid_file" ] || continue
            _pid=$(sed -n '1p' "$_pid_file")
            soak_bounded_kill "$_pid" TERM || _leftovers="$_leftovers pid:$_pid"
        done
    fi
    if [ -n "$_leftovers" ]; then
        echo "ordinary cleanup could not remove:$_leftovers" | tee -a "$_run/failure-summary.txt"
        return 1
    fi
    return 0
}

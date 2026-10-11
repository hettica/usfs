#!/usr/bin/env bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Run AIX tests over SSH and download a coverage evidence archive.
# Usage: bash collect-coverage.sh USER@HOST REMOTE-WORKSPACE [OUTPUT-DIRECTORY]

set -euo pipefail

readonly connection_timeout_seconds=10
readonly default_ssh_port=22

usage()
{
    echo 'Usage: bash collect-coverage.sh USER@HOST REMOTE-WORKSPACE [OUTPUT-DIRECTORY]'
    echo 'REMOTE-WORKSPACE is an absolute parent directory; every run uses a new child.'
    echo 'SSH_PORT selects the port; SSH configuration supplies keys and host verification.'
    echo 'AIX_PATH optionally supplies additional remote tool directories.'
    echo 'The remote account must be root. All builds, tests and reporting run on AIX.'
}

validate_arguments()
{
    if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
        usage >&2
        return 2
    fi

    if [[ ! "$1" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]*@[A-Za-z0-9_][A-Za-z0-9_.-]*$ ]]; then
        echo 'Failed to select AIX host: expected USER@HOST (HOST may be an SSH alias)' >&2
        return 2
    fi

    # These paths also enter AIX shell commands and native test fixture commands.
    # Keep the remote workspace free of shell metacharacters and whitespace.
    case "$2" in
        /) echo 'Failed to select workspace: use a dedicated absolute parent directory' >&2; return 2 ;;
        /*) ;;
        *) echo 'Failed to select workspace: expected an absolute AIX path' >&2; return 2 ;;
    esac
    case "$2" in
        *[!A-Za-z0-9_./-]*|*//*|*/../*|*/..|*/./*|*/.)
            echo 'Failed to select workspace: use a normalized path containing letters, digits, /, _, . or -' >&2
            return 2
            ;;
    esac
}

require_local_tools()
{
    for tool in ssh scp git tar gzip mktemp; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "Failed to prepare source transfer: missing local tool $tool" >&2
            return 1
        fi
    done
}

prepare_source_archive()
{
    local destination=$1
    # Include the current working tree, including nonignored new files, without
    # transferring Git credentials, ignored outputs, or an existing build tree.
    git ls-files --cached --others --exclude-standard -z |
        while IFS= read -r -d '' path; do
            if [ -e "$path" ] || [ -L "$path" ]; then
                printf '%s\0' "$path"
            fi
        done |
        tar --null -T - -czf "$destination"
}

configure_run()
{
    repository_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
    cd "$repository_root"

    remote_tool_path=${AIX_PATH:-/opt/freeware/bin:/usr/bin:/usr/sbin:/etc}
    case "$remote_tool_path" in
        *[!A-Za-z0-9_./:-]*|'')
            echo 'Failed to select AIX tool path: use a colon-separated directory list without whitespace' >&2
            return 2
            ;;
    esac

    remote_host=$1
    remote_parent=${2%/}
    output_directory=${3:-$repository_root/out/coverage}
    ssh_port=${SSH_PORT:-$default_ssh_port}
    if [[ ! "$ssh_port" =~ ^[0-9]+$ ]] || [ "${#ssh_port}" -gt 5 ]; then
        echo 'Failed to select SSH port: expected an integer from 1 to 65535' >&2
        return 2
    fi
    if [ "$((10#$ssh_port))" -lt 1 ] || [ "$((10#$ssh_port))" -gt 65535 ]; then
        echo 'Failed to select SSH port: expected an integer from 1 to 65535' >&2
        return 2
    fi

    run="coverage-$(TZ=UTC date '+%Y%m%dT%H%M%SZ')-$$"
    remote_run="$remote_parent/$run"
    archive_name="usfs-$run.tar.gz"
    revision=$(git rev-parse HEAD)
    dirty=no
    [ -z "$(git status --porcelain)" ] || dirty=yes
    ssh_options=(-p "$ssh_port" -o BatchMode=yes -o ConnectTimeout="$connection_timeout_seconds")
    scp_options=(-P "$ssh_port" -o BatchMode=yes -o ConnectTimeout="$connection_timeout_seconds")
}

preflight_host()
{
    # Preflight happens before source upload or any installed-USFS changes.
    ssh "${ssh_options[@]}" "$remote_host" "export PATH='$remote_tool_path':\$PATH;"'
        set -eu
        export PATH=/opt/freeware/bin:/usr/bin:/usr/sbin:/etc:$PATH
        if [ "$(uname -s)" != AIX ]; then
            echo "Failed to select test host: AIX is required" >&2
            exit 1
        fi
        if [ "$(id -u)" -ne 0 ]; then
            echo "Failed to run AIX tests: the SSH account must be root" >&2
            exit 1
        fi
        for tool in bash cmake make gcc gcov lcov genhtml perl tar gzip; do
            command -v "$tool" >/dev/null || { echo "Missing AIX prerequisite: $tool" >&2; exit 1; }
        done
        if ! gcov --version | sed -n "1p" | grep -q 13.3.0; then
            echo "Failed to prepare coverage: gcov 13.3.0 is required" >&2
            exit 1
        fi
        if mount | awk '\''$3 == "usfs" { found=1 } END { exit !found }'\''; then
            echo "Failed to reserve test host: an existing USFS mount is active" >&2
            exit 1
        fi
        if ps -ef | grep -E "[u]sfs_(mirror|memfs|scenario_daemon|mt_test_daemon)" >/dev/null; then
            echo "Failed to reserve test host: an existing USFS daemon is active" >&2
            exit 1
        fi
    '
}

prepare_local_snapshot()
{
    temporary_directory=$(mktemp -d)
    trap "$(printf 'rm -rf -- %q' "$temporary_directory")" EXIT
    prepare_source_archive "$temporary_directory/source.tar.gz"
    mkdir -p "$output_directory"
    if [ -e "$output_directory/$archive_name" ]; then
        echo "Failed to reserve local archive: $output_directory/$archive_name already exists" >&2
        return 1
    fi
}

transfer_source()
{
    ssh "${ssh_options[@]}" "$remote_host" "set -e; mkdir -p '$remote_parent'; mkdir '$remote_run'; mkdir '$remote_run/source'"
    scp "${scp_options[@]}" "$temporary_directory/source.tar.gz" "$remote_host:$remote_run/source.tar.gz"
    ssh "${ssh_options[@]}" "$remote_host" "export PATH='$remote_tool_path':\$PATH; bash -e -o pipefail -c \"cd '$remote_run/source'; gzip -dc '../source.tar.gz' | tar -xf -\""
}

run_remote_campaign()
{
    test_status=0
    ssh "${ssh_options[@]}" "$remote_host" \
        "export PATH='$remote_tool_path':\$PATH; bash '$remote_run/source/scripts/aix/collect-coverage.sh' '$remote_run' '$run' '$revision' '$dirty'" || test_status=$?

    if ! ssh "${ssh_options[@]}" "$remote_host" "test -s '$remote_run/$archive_name'"; then
        echo "Failed to download coverage: no archive was produced; inspect $remote_run on $remote_host" >&2
        return 1
    fi
}

download_results()
{
    echo "Downloading $archive_name to $output_directory (AIX workflow status: $test_status)"
    scp "${scp_options[@]}" "$remote_host:$remote_run/$archive_name" "$output_directory/$archive_name"
}

main()
{
    if [ "${1:-}" = --help ] || [ "${1:-}" = -h ]; then
        usage
        return 0
    fi

    validate_arguments "$@"
    require_local_tools
    configure_run "$@"
    preflight_host
    prepare_local_snapshot
    transfer_source
    run_remote_campaign
    download_results
    return "$test_status"
}

main "$@"

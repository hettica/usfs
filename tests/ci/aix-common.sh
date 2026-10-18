#!/usr/bin/env bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Share environment validation, SSH, and source transfer for AIX CI.
# Usage: Source this file from a runner script; it does not start any work.

readonly EXIT_CONFIGURATION_ERROR=2
readonly AIX_SOURCE_DIR=/home/ws
readonly RUNNER_SOURCE_DIR=${GITHUB_WORKSPACE:?GITHUB_WORKSPACE is required.}

require_environment_variable()
{
    local variable_name=$1

    if [[ -n "${!variable_name:-}" ]]; then
        return
    fi

    echo "Configure $variable_name in the workflow environment before running CI." >&2
    exit "$EXIT_CONFIGURATION_ERROR"
}

load_aix_ssh_options()
{
    require_environment_variable AIX_HOST
    require_environment_variable AIX_PORT
    require_environment_variable AIX_USER
    require_environment_variable AIX_SSH_OPTIONS

    read -r -a aix_ssh_options <<< "$AIX_SSH_OPTIONS"
    aix_ssh_options+=(-p "$AIX_PORT")
    return "$?"
}

run_aix_ssh_command()
{
    ssh "${aix_ssh_options[@]}" "$AIX_USER@$AIX_HOST" "$@"
    return "$?"
}

upload_sources_to_aix()
{
    run_aix_ssh_command "mkdir -p '$AIX_SOURCE_DIR'"

    tar -C "$RUNNER_SOURCE_DIR" --exclude=.git -cf - . |
        run_aix_ssh_command "cd '$AIX_SOURCE_DIR' && tar -xf -"
    return "$?"
}

configure_aix_build()
{
    run_aix_ssh_command "
        set -eu
        cd '$AIX_SOURCE_DIR'
        cmake -S . -B build
    "
    return "$?"
}

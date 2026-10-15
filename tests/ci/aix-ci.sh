#!/usr/bin/env bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Copy the checkout to AIX and run packaging and test gates over SSH.
#
# Usage:
#   bash tests/ci/aix-ci.sh
#   Requires GITHUB_WORKSPACE, AIX_HOST, AIX_PORT, AIX_USER,
#   and AIX_SSH_OPTIONS.

set -euo pipefail

require_environment_variable()
{
    local variable_name=$1

    if [ -n "${!variable_name:-}" ]; then
        return
    fi

    echo "Configure GitHub secret $variable_name before running AIX CI." >&2
    exit 2
}

run_on_aix()
{
    ssh "${ssh_options[@]}" "$AIX_USER@$AIX_HOST" "$@"
}

prepare_aix_workspace()
{
    run_on_aix "mkdir -p '$AIX_WORKSPACE'"
}

copy_source_workspace()
{
    tar -C "$SOURCE_WORKSPACE" --exclude=.git -cf - . |
        run_on_aix "cd '$AIX_WORKSPACE' && tar -xf -"
}

run_aix_tests()
{
    run_on_aix "
        set -eu
        cd '$AIX_WORKSPACE'
        cmake -S . -B build
        cd build
        make
        make test
        make test-e2e
    "
}

require_environment_variable AIX_HOST
require_environment_variable AIX_PORT
require_environment_variable AIX_USER
require_environment_variable AIX_SSH_OPTIONS

readonly AIX_HOST
readonly AIX_PORT
readonly AIX_USER
readonly AIX_SSH_OPTIONS
readonly AIX_WORKSPACE=/home/ws
readonly SOURCE_WORKSPACE=${GITHUB_WORKSPACE:?GITHUB_WORKSPACE is required.}

read -r -a ssh_options <<< "$AIX_SSH_OPTIONS"
ssh_options+=(-p "$AIX_PORT")

prepare_aix_workspace
copy_source_workspace
run_aix_tests

#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

set -eu

macro_pattern='^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif).*(USFS_TESTING|USFS_COVERAGE)'
macro_matches=$(
    find src \
        -path 'src/kernel/instrumentation' -prune -o \
        -path 'src/common/instrumentation' -prune -o \
        -type f \( -name '*.c' -o -name '*.h' \) \
        -exec grep -HnE "$macro_pattern" {} + || true
)

direct_pattern='usfs_test_|USFS_TEST_'
direct_matches=$(
    find src/kernel \
        -path 'src/kernel/instrumentation' -prune -o \
        -type f \( -name '*.c' -o -name '*.h' \) \
        -exec grep -HnE "$direct_pattern" {} + || true
)

test_dependency_pattern='(^|[[:space:]"<])(tests/|fake_kernel([.]h)?([>"[:space:]]|$))'
test_dependency_matches=$(
    find src \
        -type f \( -name '*.c' -o -name '*.h' \) \
        -exec grep -HnE "$test_dependency_pattern" {} + || true
)

if [ -n "$macro_matches" ] || [ -n "$direct_matches" ] || \
    [ -n "$test_dependency_matches" ]; then
    printf '%s\n' "kernel instrumentation boundary violation:" >&2
    if [ -n "$macro_matches" ]; then
        printf '%s\n' "$macro_matches" >&2
    fi
    if [ -n "$direct_matches" ]; then
        printf '%s\n' "$direct_matches" >&2
    fi
    if [ -n "$test_dependency_matches" ]; then
        printf '%s\n' "$test_dependency_matches" >&2
    fi
    exit 1
fi

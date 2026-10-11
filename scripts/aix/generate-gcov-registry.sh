#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Inspect AIX kernel coverage objects, validate their gcov constructor ABI,
#   and generate the C registry that explicitly invokes those constructors in
#   a kernel extension where ordinary process startup hooks do not run.
#
# Usage:
#   Run on AIX with an output C path followed by instrumented objects:
#     bash scripts/aix/generate-gcov-registry.sh \
#       out/generated/usfs_gcov_registry.c out/obj/kernel/state.o

set -eu

if [ "$#" -lt 2 ]; then
    echo "usage: $0 OUTPUT_C OBJECT..." >&2
    exit 2
fi

output=$1
shift
temporary="${output}.tmp"
constructors=""
count=0

for object in "$@"; do
    symbols=$(nm -X64 -B "$object")
    lpbx_count=$(printf '%s\n' "$symbols" | grep -c ' LPBX..0$' || true)
    ctor_count=$(printf '%s\n' "$symbols" | grep -c 'T ._GLOBAL__I_' || true)
    dtor_count=$(printf '%s\n' "$symbols" | grep -c 'T ._GLOBAL__D_' || true)
    if [ "$lpbx_count" -ne 1 ] || [ "$ctor_count" -ne 1 ] ||
       [ "$dtor_count" -ne 1 ]; then
        echo "$object: expected one LPBX..0, constructor, and destructor; got $lpbx_count/$ctor_count/$dtor_count" >&2
        exit 1
    fi
    unresolved=$(nm -X64 -u "$object" |
        awk '/__gcov_/ { symbol=$1; sub(/^\./, "", symbol); print symbol }')
    for required in __gcov_init __gcov_exit __gcov_merge_add; do
        required_count=$(printf '%s\n' "$unresolved" |
            grep -c "^${required}$" || true)
        if [ "$required_count" -ne 1 ]; then
            echo "$object: expected one unresolved $required" >&2
            exit 1
        fi
    done
    unexpected=$(printf '%s\n' "$unresolved" |
        grep -Ev '^(__gcov_init|__gcov_exit|__gcov_merge_add)$' || true)
    if [ -n "$unexpected" ]; then
        echo "$object: unexpected gcov runtime imports:" >&2
        printf '%s\n' "$unexpected" >&2
        exit 1
    fi
    constructor=$(printf '%s\n' "$symbols" |
        awk '/T ._GLOBAL__I_/ {print $3}' | sed 's/^\.//')
    if [ -z "$constructor" ]; then
        echo "$object: cannot extract constructor symbol" >&2
        exit 1
    fi
    constructors="${constructors}${constructor}
"
    count=$((count + 1))
done

{
    echo "/* Generated from instrumented XCOFF objects. Do not edit. */"
    printf '%s' "$constructors" | while IFS= read -r constructor; do
        [ -n "$constructor" ] &&
            echo "extern void ${constructor} (void);"
    done
    echo
    echo "void usfs_gcov_register_all (void)"
    echo "{"
    printf '%s' "$constructors" | while IFS= read -r constructor; do
        [ -n "$constructor" ] &&
            echo "    ${constructor} ();"
    done
    echo "}"
    echo
    echo "unsigned usfs_gcov_expected_units (void)"
    echo "{"
    echo "    return ${count}u;"
    echo "}"
} > "$temporary"

mv "$temporary" "$output"
echo "generated $output for $count gcov units"

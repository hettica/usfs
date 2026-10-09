#!/usr/bin/env bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Validate every repository shell source with bash -n and reject accidental
#   Windows-host dependencies in portable project files.
#
# Usage:
#   From the AIX source directory:
#     bash tests/meta/source_validation.sh
#   The normal entry point is `make test` in the CMake binary directory.
#
# Notes:
#   The script emits a five-case TAP stream and scans both scripts/ and tests/.
#   Keep its self-exclusion path synchronized if this file moves.

set -u

failures=0
checked=0

for file in $(find scripts tests -type f \( -name '*.sh' -o -name '*.bash' \) -print); do
    checked=$((checked + 1))
    if ! bash -n "$file"; then
        echo "shell syntax invalid: $file"
        failures=$((failures + 1))
    fi
    case "$file" in
        tests/*)
            if ! grep -q '^# Purpose:' "$file" ||
               ! grep -q '^# Usage:' "$file"; then
                echo "test shell header missing Purpose or Usage: $file"
                failures=$((failures + 1))
            fi
            ;;
    esac
done

if grep -E -i -n \
   'C:[\\/]|cygwin|powershell|cmd\.exe|wsl\.exe' \
   CMakeLists.txt $(find scripts tests src -type f \
       ! -path 'tests/meta/source_validation.sh' -print) \
   >/tmp/usfs-windows-reference.$$ 2>/dev/null; then
    echo "Windows-host dependency found:"
    sed 's/^/# /' /tmp/usfs-windows-reference.$$
    failures=$((failures + 1))
fi
rm -f /tmp/usfs-windows-reference.$$

echo "TAP version 13"
echo "1..5"
if [ "$failures" -eq 0 ]; then
    echo "ok 1 - shell sources parse and contain no Windows-host dependencies"
else
    echo "not ok 1 - shell sources parse and contain no Windows-host dependencies"
fi
if [ "$checked" -gt 0 ]; then
    echo "ok 2 - shell validation discovered $checked scripts"
else
    echo "not ok 2 - shell validation discovered scripts"
    failures=$((failures + 1))
fi

if grep -q 'USFS_TRACE_CONTROL_HOOK 0xF5F10000UL' \
        src/kernel/instrumentation/trace.h &&
   grep -q 'TRCHKL5T' src/kernel/instrumentation/trace.h &&
   ! grep -R -q 'USFS_IOC_GETLOG\|USFS_LOGF\|_log_drain' \
        src/kernel src/common/usfs_proto.h; then
    echo "ok 3 - kernel diagnostics use the fixed native AIX trace ABI"
else
    echo "not ok 3 - kernel diagnostics use the fixed native AIX trace ABI"
    failures=$((failures + 1))
fi

if ! grep -R -q 'USFS_CANCELED_SLOTS\|canceled_unique' \
        src/kernel/definitions.h src/kernel/device &&
   grep -q 'state == USFS_REQ_SENT' src/kernel/device/protocol/request_state.c &&
   grep -q 'USFS_REQUEST_INTERRUPT_COMPLETE' src/kernel/device/protocol/request_state.c &&
   grep -q 'noninterruptible = true' src/kernel/device/protocol/request.c &&
   grep -R -q -- '->abandoned' src/kernel/device; then
    echo "ok 4 - delivered requests retain completion ownership through signals"
else
    echo "not ok 4 - delivered requests retain completion ownership through signals"
    failures=$((failures + 1))
fi

if [ -n "${USFS_BINARY_DIR:-}" ]; then
    freshness_failure=0
    for variant in release testing; do
        if [ "$variant" = release ]; then
            image="$USFS_BINARY_DIR/bin/usfs.kext"
            object_groups="release_objects hooks"
        else
            image="$USFS_BINARY_DIR/testing/bin/usfs.kext"
            object_groups="testing_core testing_instrumentation hooks"
        fi
        if [ ! -s "$image" ]; then
            echo "# missing $variant kernel image"
            freshness_failure=1
            continue
        fi
        for group in $object_groups; do
            object_directory="$USFS_BINARY_DIR/CMakeFiles/_usfs_kernel_$group.dir"
            if [ ! -d "$object_directory" ]; then
                freshness_failure=1
                continue
            fi
            newer=$(find "$object_directory" -type f -name '*.o' -newer "$image" -print) || freshness_failure=1
            if [ -n "$newer" ]; then
                echo "# stale $variant image; newer objects follow"
                printf '%s\n' "$newer" | sed 's/^/# /'
                freshness_failure=1
            fi
        done
    done
    if [ "$freshness_failure" -eq 0 ]; then
        echo "ok 5 - linked kernels include the current compiled objects"
    else
        echo "not ok 5 - linked kernels include the current compiled objects"
        failures=$((failures + 1))
    fi
else
    echo "ok 5 - artifact freshness requires the CMake test target # SKIP standalone source inspection"
fi

[ "$failures" -eq 0 ]

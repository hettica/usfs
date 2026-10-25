#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Only for isolated, explicitly authorized test environments. Never called by RPM.
# The caller must stop/join workers and unconfigure the implementation first.
set -eu
build=${1:?build directory required}
variant=${2:?destination variant required}
case "$variant" in
    release) target="$build/bin/usfs_proxy.kext" ;;
    testing|coverage) target="$build/$variant/bin/usfs_proxy.kext" ;;
    *) echo "unknown proxy variant: $variant" >&2; exit 2 ;;
esac
[ -s "$target" ] || { echo "missing proxy image: $target" >&2; exit 1; }
control="$build/bin/usfsctl"
state=$("$control" kexts)
printf '%s\n' "$state"
if printf '%s\n' "$state" | grep -q '^implementation: loaded=yes'; then
    echo 'refusing proxy variant switch while the implementation is loaded' >&2
    exit 1
fi
if printf '%s\n' "$state" | grep -q '^proxy: loaded=yes'; then
    loaded=$(printf '%s\n' "$state" | sed -n 's/^proxy: .* build=\([^ ]*\) variant=.*/\1/p')
    if printf '%s\n' "$state" | grep -q "variant=$variant" &&
       [ -n "$loaded" ] && strings -a "$target" | grep -F -q "$loaded"; then
        echo "resident proxy already matches $variant image $loaded"
        exit 0
    fi
    if mount | grep -i usfs >/dev/null 2>&1 ||
       ps -ef | grep -E '[u]sfs_(mirror|memfs|proxy_testctl|probe|testctl|scenario_daemon|mt_test_daemon|path_test_daemon)' >/dev/null 2>&1; then
        echo 'refusing unsafe proxy change while USFS test workers or mounts remain' >&2
        exit 1
    fi
    if printf '%s\n' "$state" | grep -q 'variant=coverage'; then
        destination="$build/coverage/proxy-switch-$(date -u +%Y%m%dT%H%M%S)-$$"
        "$build/coverage/bin/usfs_coverage" proxy-snapshot "$destination"
    fi
    echo 'ISOLATED TEST MAINTENANCE: explicitly confirmed unsafe proxy removal' >&2
    "$control" proxy unload --unsafe --confirm-unsafe-unload
fi

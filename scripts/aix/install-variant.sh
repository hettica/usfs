#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose: Install one already-built USFS CMake variant directly on AIX.
# Usage: bash scripts/aix/install-variant.sh release|testing source build

set -eu

variant=$1
source_root=$2
build_root=$3

test "$(id -u)" -eq 0 || { echo "USFS live targets require root" >&2; exit 1; }

mkdir -p /usr/lib/drivers/usfs /usr/lib/methods/usfs /usr/lib/objrepos \
    /usr/lib/nls/msg/en_US /usr/share/doc/usfs /usr/lib/ras /usr/sbin
cp "$source_root/packaging/aix/rpm/package_lifecycle.sh" \
    /usr/lib/methods/usfs/package_lifecycle
cp "$source_root/packaging/aix/rpm/boot_rule.sh" /usr/lib/methods/usfs/boot_rule
chmod 550 /usr/lib/methods/usfs/package_lifecycle /usr/lib/methods/usfs/boot_rule
cp "$source_root/packaging/aix/odm/usfs.pddv.add" \
    "$source_root/packaging/aix/odm/usfs.config_rules.add" \
    "$source_root/packaging/aix/odm/usfs.err" \
    "$source_root/packaging/aix/odm/usfs.err.undo" /usr/lib/objrepos/
cp "$source_root/packaging/aix/trace/usfs.trcfmt" \
    "$source_root/packaging/aix/trace/usfs_trace.trc" \
    "$source_root/packaging/aix/trace/usfs_trace_remove.trc" /usr/lib/ras/
errupdate -f -q /usr/lib/objrepos/usfs.err
/usr/lib/methods/usfs/package_lifecycle register-trace
cp "$source_root/LICENSE" /usr/share/doc/usfs/LICENSE
gencat /usr/lib/nls/msg/en_US/usfs.cat "$source_root/packaging/aix/odm/usfs.msg"

case "$variant" in
    release)
        kernel="$build_root/bin/usfs.kext"
        methods="$build_root/bin"
        examples="$build_root/bin"
        rm -f /usr/sbin/usfs_testctl /usr/sbin/usfs_scenario_daemon \
            /usr/sbin/usfs_usercopy_test /usr/sbin/usfs_io_probe /usr/sbin/usfs_fid_probe \
            /usr/sbin/usfs_access_probe /usr/sbin/usfs_mt_test_daemon /usr/sbin/usfs_path_test_daemon \
            /usr/sbin/usfs_coverage
        ;;
    testing)
        kernel="$build_root/testing/bin/usfs.kext"
        methods="$build_root/testing/bin"
        examples="$build_root/bin"
        rm -f /usr/sbin/usfs_coverage
        ;;
    *) echo "unknown install variant: $variant" >&2; exit 2 ;;
esac

cp "$kernel" /usr/lib/drivers/usfs/usfs.kext
cp "$methods/cfgusfs" "$methods/ucfgusfs" "$methods/chusfs" /usr/lib/methods/usfs/
cp "$examples/usfs_mirror" "$examples/usfs_memfs" /usr/sbin/
cp "$build_root/bin/usfs_probe" "$build_root/bin/usfsctl" /usr/sbin/
rm -f /usr/sbin/usfs_logdump

if [ "$variant" != release ]; then
    cp "$build_root/testing/bin/usfs_testctl" \
        "$build_root/testing/bin/usfs_scenario_daemon" \
        "$build_root/testing/bin/usfs_usercopy_test" \
        "$build_root/testing/bin/usfs_io_probe" \
        "$build_root/testing/bin/usfs_fid_probe" \
        "$build_root/testing/bin/usfs_access_probe" \
        "$build_root/testing/bin/usfs_mt_test_daemon" \
        "$build_root/testing/bin/usfs_path_test_daemon" /usr/sbin/
fi

ln -sf /usr/lib/methods/usfs/chusfs /usr/sbin/chusfs
chmod 550 /usr/lib/drivers/usfs/usfs.kext /usr/lib/methods/usfs/*
chmod 555 /usr/sbin/usfs_mirror /usr/sbin/usfs_memfs \
    /usr/sbin/usfs_probe /usr/sbin/usfsctl
[ "$variant" = release ] || chmod 555 /usr/sbin/usfs_testctl \
    /usr/sbin/usfs_scenario_daemon /usr/sbin/usfs_usercopy_test \
    /usr/sbin/usfs_io_probe /usr/sbin/usfs_fid_probe /usr/sbin/usfs_access_probe \
    /usr/sbin/usfs_mt_test_daemon /usr/sbin/usfs_path_test_daemon
chmod 444 /usr/lib/objrepos/usfs.pddv.add \
    /usr/lib/objrepos/usfs.config_rules.add /usr/lib/objrepos/usfs.err \
    /usr/lib/objrepos/usfs.err.undo /usr/share/doc/usfs/LICENSE
chmod 444 /usr/lib/ras/usfs.trcfmt /usr/lib/ras/usfs_trace.trc \
    /usr/lib/ras/usfs_trace_remove.trc
chown root:system /usr/lib/drivers/usfs/usfs.kext /usr/lib/methods/usfs/* \
    /usr/lib/objrepos/usfs.pddv.add /usr/lib/objrepos/usfs.config_rules.add \
    /usr/lib/objrepos/usfs.err /usr/lib/objrepos/usfs.err.undo \
    /usr/lib/ras/usfs.trcfmt /usr/lib/ras/usfs_trace.trc \
    /usr/lib/ras/usfs_trace_remove.trc \
    /usr/share/doc/usfs/LICENSE

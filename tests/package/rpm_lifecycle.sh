#!/usr/bin/env bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Exercise the production AIX RPM through prerequisites, install, upgrade,
#   rollback, interrupted-operation repair, integrity, reboot, and uninstall.
#
# Usage:
#   From an AIX checkout as root:
#     USFS_BINARY_DIR=/path/to/configured/build bash tests/package/rpm_lifecycle.sh

set -u
AIX_VERSION=$(uname -v).$(uname -r)

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
BUILD_ROOT=${USFS_PACKAGE_TEST_BUILD:-$ROOT/build/package-lifecycle}
TEST_FAILED=0
TEST_NUMBER=0
RESTORE_NEEDED=1

remote()
{
    bash -c "$1"
}

foreign_trace_collision()
(
    foreign=$ROOT/tests/fixtures/trace/foreign
    foreign_remove=$ROOT/tests/fixtures/trace/foreign_remove
    restored=0

    cleanup_foreign()
    {
        /usr/bin/trcupdate -o "$foreign_remove" >/dev/null 2>&1 || true
        if /usr/lib/methods/usfs/package_lifecycle register-trace \
             >/dev/null 2>&1; then
            restored=1
        fi
    }
    trap cleanup_foreign EXIT HUP INT TERM
    /usr/bin/trcupdate -o /usr/lib/ras/usfs_trace_remove >/dev/null || return 1
    /usr/bin/trcupdate -o "$foreign" >/dev/null || return 1
    if /usr/lib/methods/usfs/package_lifecycle register-trace \
         >/dev/null 2>&1; then
        return 1
    fi
    cleanup_foreign
    trap - EXIT HUP INT TERM
    [ "$restored" -eq 1 ]
)

build_release()
{
    name=$1
    release=$2
    revision=$3
    shift 3
    directory="$BUILD_ROOT/$name"
    rm -rf "$directory"
    echo "# building package fixture $name" >&2
    cmake -S "$ROOT" -B "$directory" \
        -DUSFS_PACKAGE_RELEASE="$release" \
        -DUSFS_BUILD_REVISION="$revision" -DUSFS_BUILD_DIRTY=no "$@" || return
    cmake --build "$directory" --target package
}

# Real RPM transactions against retained registrations on the disposable AIX
# host. The existing testing build supplies faults without shipping test hooks.
retained_registration_case()
(
    set -eu
    kind=$1
    case_root="$BUILD_ROOT/recovery-$kind"
    methods=/usr/lib/methods/usfs
    mkdir -p "$case_root"
    if [ "$kind" = cleanup ]; then
        test -x "${USFS_BINARY_DIR:?}/testing/bin/cfgusfs"
        "$methods/ucfgusfs" -l usfs0
        bash "$ROOT/scripts/aix/install-variant.sh" testing "$ROOT" "$USFS_BINARY_DIR"
        if USFS_TEST_INIT_FAULT=18 USFS_TEST_INIT_ERRNO=16 mkdev -l usfs0 \
            >"$case_root/init.log" 2>&1; then exit 1; fi
        test -c /dev/usfs0
    else
        # DEFINED=0 and AVAILABLE=1 in AIX /usr/include/sys/cfgdb.h.
        # Change only ODM's view, retaining the active production registration.
        odmget -q 'name=usfs0' CuDv >"$case_root/device"
        sed 's/status = 1/status = 0/' "$case_root/device" >"$case_root/defined"
        odmchange -o CuDv -q 'name=usfs0' "$case_root/defined"
        /usr/sbin/usfsctl >"$case_root/active.log"
        grep -q '^kext: ACTIVE$' "$case_root/active.log"
    fi
    test "$(lsdev -l usfs0 -F status)" = Defined
    cp -p "$methods/ucfgusfs" "$case_root/ucfgusfs"
    restore_case()
    {
        status=$?
        trap - EXIT HUP INT TERM
        cp -p "$case_root/ucfgusfs" "$methods/ucfgusfs" || exit 1
        "$methods/ucfgusfs" -l usfs0 || exit 1
        rpm -Uvh --replacepkgs "$R1" >"$case_root/restore.log" 2>&1 || exit 1
        exit "$status"
    }
    trap restore_case EXIT HUP INT TERM
    # Reproduce the current legacy helper's pre-install behavior. The incoming
    # RPM must perform its own teardown rather than relying on this helper.
    cat >"$methods/package_lifecycle" <<'EOF'
#!/usr/bin/ksh
line=$(/usr/sbin/lsdev -l usfs0 2>/dev/null)
if echo "$line" | grep -q Available; then /usr/sbin/rmdev -l usfs0 || exit 1; fi
exit 0
EOF
    chmod 550 "$methods/package_lifecycle"
    if [ "$kind" = missing ]; then
        chmod 440 "$methods/ucfgusfs"
    elif [ "$kind" = failed ]; then
        cat >"$methods/ucfgusfs" <<'EOF'
#!/usr/bin/ksh
exit 1
EOF
        chmod 550 "$methods/ucfgusfs"
    fi
    if [ "$kind" = missing ] || [ "$kind" = failed ]; then
        perl "$ROOT/scripts/aix/sha256.pl" /usr/lib/drivers/usfs/usfs.kext \
            "$methods/ucfgusfs" "$methods/package_lifecycle" >"$case_root/before"
        rpm -q usfs >"$case_root/version-before"
        if rpm -Uvh --replacepkgs "$R1" >"$case_root/blocked.log" 2>&1; then exit 1; fi
        perl "$ROOT/scripts/aix/sha256.pl" /usr/lib/drivers/usfs/usfs.kext \
            "$methods/ucfgusfs" "$methods/package_lifecycle" >"$case_root/after"
        rpm -q usfs >"$case_root/version-after"
        cmp "$case_root/before" "$case_root/after"
        cmp "$case_root/version-before" "$case_root/version-after"
        test "$(lsdev -l usfs0 -F status)" = Defined
        cp -p "$case_root/ucfgusfs" "$methods/ucfgusfs"
    fi
    rpm -Uvh --replacepkgs "$R1" >"$case_root/replace.log" 2>&1
    # Successful replacement installed production methods; keep the trap's
    # teardown matched to that image, including after the testing-residue case.
    cp -p "$methods/ucfgusfs" "$case_root/ucfgusfs"
    rpm -V usfs
    /usr/sbin/usfsctl >"$case_root/status.log"
    grep -q '^USFS health: HEALTHY$' "$case_root/status.log"
    # Replacement has already restored and verified the ordinary RPM.
    trap - EXIT HUP INT TERM
)

activate_release()
{
    directory=$1
    bash "$ROOT/scripts/aix/cleanup.sh" >/dev/null 2>&1 || return 1
    if [ -x /usr/lib/methods/usfs/ucfgusfs ]; then
        /usr/lib/methods/usfs/ucfgusfs -l usfs0 >/dev/null || return 1
    fi
    bash "$ROOT/scripts/aix/install-variant.sh" release "$ROOT" "$directory" || return
    bash "$ROOT/scripts/aix/setup.sh" >/dev/null
}

ok()
{
    TEST_NUMBER=$((TEST_NUMBER + 1))
    echo "ok $TEST_NUMBER - $1"
}

not_ok()
{
    TEST_NUMBER=$((TEST_NUMBER + 1))
    echo "not ok $TEST_NUMBER - $1"
    TEST_FAILED=1
}

check()
{
    description=$1
    shift
    # Do not suppress errexit inside transaction-test subshells.
    "$@"
    result=$?
    if [ "$result" -eq 0 ]; then
        ok "$description"
    else
        not_ok "$description"
    fi
}

restore_development_install()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$RESTORE_NEEDED" -eq 0 ]; then
        exit "$status"
    fi
    echo "# restoring ordinary release build" >&2
    if rpm -q usfs >/dev/null 2>&1; then
        rpm -e usfs || exit 1
    fi
    activate_release "${USFS_BINARY_DIR:-$BUILD_ROOT/r1}" || exit 1
    exit "$status"
}

trap restore_development_install EXIT HUP INT TERM

echo "TAP version 13"
echo "1..24"

mkdir -p "$BUILD_ROOT"
build_release r1 1 test-r1 >/dev/null || exit 1
build_release r2 2 test-r2 >/dev/null || exit 1
build_release interrupted 3 test-interrupted \
    -DUSFS_PACKAGE_PRE_FAULT=after-unconfigure >/dev/null || exit 1
build_release r3 3 test-r3 >/dev/null || exit 1
build_release prerequisite 99 test-prereq \
    -DUSFS_PACKAGE_EXTRA_REQUIRES=usfs-release-test-prerequisite >/dev/null || exit 1
cp "$BUILD_ROOT/interrupted/packages/usfs-0.1.0-3.aix$AIX_VERSION.ppc.rpm" \
    "$BUILD_ROOT/interrupted/packages/usfs-0.1.0-3.interrupted.aix$AIX_VERSION.ppc.rpm"
activate_release "$BUILD_ROOT/r1" >/dev/null || exit 1

R1=$BUILD_ROOT/r1/packages/usfs-0.1.0-1.aix$AIX_VERSION.ppc.rpm
R2=$BUILD_ROOT/r2/packages/usfs-0.1.0-2.aix$AIX_VERSION.ppc.rpm
R3=$BUILD_ROOT/r3/packages/usfs-0.1.0-3.aix$AIX_VERSION.ppc.rpm
R3_INTERRUPTED=$BUILD_ROOT/interrupted/packages/usfs-0.1.0-3.interrupted.aix$AIX_VERSION.ppc.rpm
R99=$BUILD_ROOT/prerequisite/packages/usfs-0.1.0-99.aix$AIX_VERSION.ppc.rpm

RPM_REQUIRED="/usr/lib/drivers/usfs/usfs.kext /usr/lib/methods/usfs/package_lifecycle /usr/lib/ras/usfs.trcfmt /usr/sbin/usfsctl /usr/share/doc/usfs/build-manifest.txt"
check "RPM headers and payload tables are valid" remote \
    "perl $ROOT/scripts/aix/verify-rpm.pl $R1 0.1.0 1 $RPM_REQUIRED && perl $ROOT/scripts/aix/verify-rpm.pl $R2 0.1.0 2 $RPM_REQUIRED && perl $ROOT/scripts/aix/verify-rpm.pl $R3 0.1.0 3 $RPM_REQUIRED"
check "package declares its AIX prerequisite" remote "rpm -qpR $R1 | grep -q 'AIX-rpm >= 7.2.0.0'"
check "missing prerequisite blocks the RPM transaction" remote "! rpm -U --test $R99 >/tmp/usfs-prereq.log 2>&1 && grep -q usfs-release-test-prerequisite /tmp/usfs-prereq.log"

check "release 1 installs and configures usfs0" remote "rpm -Uvh --replacepkgs $R1 >/tmp/usfs-rpm-install.log 2>&1 && rpm -q usfs | grep -q 'usfs-0.1.0-1' && lsdev -l usfs0 | grep -q Available"
check "installed usfsctl reports a healthy production kext" remote "/usr/sbin/usfsctl | grep -q '^USFS health: HEALTHY$'"
check "installed USFS errpt and trace templates are registered" remote "errpt -t -j 367CFADB,03CB297B,0B6DD74B,BDAF4D05,5D669390 | grep -q USFS_TIMEOUT && grep -q '^F5F1 .*\"USFS CONTROL\"' /etc/trcfmt && grep -q '^F5F2 .*\"USFS REQUEST\"' /etc/trcfmt && grep -q '^F5F3 .*\"USFS FILESYSTEM\"' /etc/trcfmt && trcrpt -c -t /etc/trcfmt >/dev/null && test ! -e /usr/sbin/usfs_logdump"
check "foreign trace hook ownership blocks replacement and is cleaned" \
    foreign_trace_collision
check "installed release 1 inventory verifies" remote "rpm -V usfs"
check "installed package enforces the control-plane policy" remote "USFS_SOURCE_DIR=$ROOT USFS_SCENARIO_DAEMON=${USFS_BINARY_DIR:?}/testing/bin/usfs_scenario_daemon bash $ROOT/tests/integration/aix/contract/control_plane_security.sh"

check "incoming RPM cleans Defined ACTIVE state despite the legacy helper" retained_registration_case active
check "incoming RPM cleans Defined CLEANUP_REQUIRED state before replacement" retained_registration_case cleanup
check "failed direct teardown preserves the installed version and payload digests" retained_registration_case failed
check "unavailable direct teardown preserves residual installation for repair" retained_registration_case missing

check "release 2 upgrades the loaded extension" remote "rpm -Uvh $R2 >/tmp/usfs-rpm-upgrade.log 2>&1 && rpm -q usfs | grep -q 'usfs-0.1.0-2' && lsdev -l usfs0 | grep -q Available"
check "retained release 1 artifact rolls back release 2" remote "rpm -Uvh --oldpackage $R1 >/tmp/usfs-rpm-rollback.log 2>&1 && rpm -q usfs | grep -q 'usfs-0.1.0-1' && lsdev -l usfs0 | grep -q Available"

check "interrupted pre-install aborts without replacing release 1" remote "! rpm -Uvh $R3_INTERRUPTED >/tmp/usfs-rpm-interrupted.log 2>&1 && rpm -q usfs | grep -q 'usfs-0.1.0-1' && lsdev -l usfs0 | grep -q Defined"
check "rerunning with the intact release repairs interrupted state" remote "rpm -Uvh $R3 >/tmp/usfs-rpm-repair.log 2>&1 && rpm -q usfs | grep -q 'usfs-0.1.0-3' && lsdev -l usfs0 | grep -q Available"

check "RPM verification detects tampering" remote "chmod 777 /usr/sbin/chusfs && ! rpm -V usfs >/tmp/usfs-rpm-verify.log 2>&1 && grep -q /usr/sbin/chusfs /tmp/usfs-rpm-verify.log"
check "replace-package repair restores package integrity" remote "rpm -Uvh --replacepkgs $R3 >/tmp/usfs-rpm-replace.log 2>&1 && rpm -V usfs"

check "packaged device can enter the boot-time Defined state" remote "rmdev -l usfs0 >/tmp/usfs-rpm-boot-unconfigure.log 2>&1 && lsdev -l usfs0 | grep -q Defined"
check "AIX cfgmgr boot discovery restores the packaged device" remote "cfgmgr >/tmp/usfs-rpm-cfgmgr.log 2>&1 && lsdev -l usfs0 | grep -q Available && set -- \$(ls -l /dev/usfs0); test \"\$1\" = crw-rw---- && test \"\$3\" = root && test \"\$4\" = system"

check "uninstall removes package, ODM, device, extension, RAS, trace, and VFS state" remote "rpm -e usfs >/tmp/usfs-rpm-erase.log 2>&1 && ! rpm -q usfs >/dev/null 2>&1 && test -z \"\$(lsdev -l usfs0 2>/dev/null)\" && test -z \"\$(odmget -q 'uniquetype=cdr/fs/usfs' PdDv 2>/dev/null)\" && test -z \"\$(odmget -q 'rule=/usr/lib/methods/usfs/boot_rule' Config_Rules 2>/dev/null)\" && test -z \"\$(errpt -t -j 367CFADB,03CB297B,0B6DD74B,BDAF4D05,5D669390 2>/dev/null)\" && ! grep -q '^F5F[123] .*\"USFS ' /etc/trcfmt && test ! -e /dev/usfs0 && test ! -e /usr/lib/drivers/usfs/usfs.kext && test ! -e /usr/sbin/usfsctl && ! grep -q '^usfs[[:space:]]' /etc/vfs"

check "verified fresh installation works without an existing teardown method" remote \
    "rpm -Uvh $R1 >/tmp/usfs-rpm-fresh.log 2>&1 && rpm -V usfs && /usr/sbin/usfsctl"
erase_defined()
{
    odmget -q 'name=usfs0' CuDv >"$BUILD_ROOT/erase-device" || return 1
    sed 's/status = 1/status = 0/' "$BUILD_ROOT/erase-device" >"$BUILD_ROOT/erase-defined"
    odmchange -o CuDv -q 'name=usfs0' "$BUILD_ROOT/erase-defined" || return 1
    rpm -e usfs >"$BUILD_ROOT/erase-defined.log" 2>&1 || return 1
    test ! -e /dev/usfs0 && test ! -e /usr/lib/drivers/usfs/usfs.kext &&
        ! grep -q '^usfs[[:space:]]' /etc/vfs
}
check "erasure cleans a retained ACTIVE registration even when ODM says Defined" erase_defined

if activate_release "${USFS_BINARY_DIR:-$BUILD_ROOT/r1}"; then
    RESTORE_NEEDED=0
else
    TEST_FAILED=1
fi

[ "$TEST_FAILED" -eq 0 ]

#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose: Exercise both production pre-install entrypoints against isolated command fixtures.
# Usage: Run through make test from an AIX CMake binary directory.
set -eu
. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"
base=/tmp/usfs-package-preinstall.$$
mkdir -p "$base/methods" "$base/drivers" "$base/dev"
trap 'rm -rf "$base"' EXIT HUP INT TERM

# Substitute paths in copies of the actual entrypoints; no installed state is touched.
rewrite()
{
    sed -e "s|/usr/lib/methods/usfs|$base/methods|g" \
        -e "s|/usr/lib/drivers/usfs|$base/drivers|g" \
        -e "s|/usr/bin/odmget|$base/odmget|g" \
        -e "s|/dev/|$base/dev/|g" -e 's|%{usfs_pre_fault}|none|g'
}
sed '/^case ${1:-}/,$d' "$USFS_SOURCE_DIR/packaging/aix/rpm/package_lifecycle.sh" |
    rewrite >"$base/helper"
echo 'pre_install none' >>"$base/helper"
awk '/^%pre -p / { copying=1; next } /^%post / { copying=0 } copying' \
    "$USFS_SOURCE_DIR/packaging/aix/rpm/usfs.spec" | rewrite >"$base/incoming"
cat >"$base/odmget" <<'EOF'
#!/usr/bin/ksh
[ "${CASE_ODM_ERROR:-0}" = 0 ] || exit 1
[ "${CASE_RECORDS:-0}" = 0 ] || echo 'CuDv: name=usfs0 status=0'
exit 0
EOF
cat >"$base/methods/package_lifecycle" <<'EOF'
#!/usr/bin/ksh
echo legacy-called >>"$CASE_LOG"
exit 97
EOF
chmod 700 "$base/odmget" "$base/methods/package_lifecycle"
CASE_LOG=$base/calls
export CASE_LOG CASE_ODM_ERROR CASE_RECORDS CASE_METHOD_ERROR
tap_plan 14
for entry in helper incoming; do
    for scenario in defined teardown-fails odm-residue driver-residue node-residue fresh query-fails; do
        CASE_ODM_ERROR=0 CASE_RECORDS=0 CASE_METHOD_ERROR=0
        rm -f "$base/methods/ucfgusfs" "$base/drivers/usfs.kext" "$base/dev/usfs0" "$CASE_LOG"
        expected=1
        case "$scenario" in
            defined|teardown-fails)
                cat >"$base/methods/ucfgusfs" <<'EOF'
#!/usr/bin/ksh
[ "$1" = -l ] && [ "$2" = usfs0 ] || exit 99
echo direct-called >>"$CASE_LOG"
exit "$CASE_METHOD_ERROR"
EOF
                chmod 700 "$base/methods/ucfgusfs"
                CASE_RECORDS=1
                if [ "$scenario" = defined ]; then expected=0; else CASE_METHOD_ERROR=1; fi
                ;;
            odm-residue) CASE_RECORDS=1 ;;
            driver-residue) touch "$base/drivers/usfs.kext" ;;
            node-residue) touch "$base/dev/usfs0" ;;
            fresh) expected=0 ;;
            query-fails) CASE_ODM_ERROR=1 ;;
        esac
        result=0
        /usr/bin/ksh "$base/$entry" >"$base/output" 2>&1 || result=1
        failure=0
        [ "$result" -eq "$expected" ] || failure=1
        if [ -f "$CASE_LOG" ]; then
            grep -q legacy-called "$CASE_LOG" && failure=1
        fi
        case "$scenario" in defined|teardown-fails) grep -q direct-called "$CASE_LOG" || failure=1 ;; esac
        tap_ok "$failure" "$entry pre-install handles $scenario without trusting the legacy helper"
    done
done
tap_finish

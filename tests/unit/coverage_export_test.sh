#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Verify portable coverage exports merge hits and escape source paths.
# Usage: Run through make coverage on AIX, or bash tests/unit/coverage_export_test.sh.

set -eu
: "${USFS_SOURCE_DIR:?USFS_SOURCE_DIR is required}"
fixture=$(perl -MFile::Temp=tempdir -e '
    print tempdir("usfs-coverage-export.XXXXXX", DIR => $ENV{TMPDIR} || "/tmp", CLEANUP => 0);
')
trap 'rm -rf "$fixture"' EXIT HUP INT TERM
mkdir "$fixture/source"
touch "$fixture/source/a&b.c"
cat >"$fixture/input.info" <<EOF
TN:
SF:$fixture/source/a&b.c
FN:1,example
FNDA:2,example
DA:1,2
DA:2,0
BRDA:1,0,0,-
BRDA:1,0,1,2
end_of_record
TN:
SF:$fixture/source/a&b.c
DA:1,0
BRDA:1,0,0,1
BRDA:1,0,1,-
end_of_record
EOF
perl "$USFS_SOURCE_DIR/scripts/aix/export-coverage.pl" "$fixture/source" \
    "$fixture/input.info" "$fixture/coverage.lcov" "$fixture/coverage.xml"
grep -qx 'SF:a&b.c' "$fixture/coverage.lcov"
grep -qx 'FNDA:2,example' "$fixture/coverage.lcov"
grep -q 'file path="a&amp;b.c"' "$fixture/coverage.xml"
grep -q 'lineNumber="1" covered="true" branchesToCover="2" coveredBranches="2"' "$fixture/coverage.xml"
grep -q 'lineNumber="2" covered="false"' "$fixture/coverage.xml"

printf 'SF:/outside/source.c\nend_of_record\n' >"$fixture/outside.info"
if perl "$USFS_SOURCE_DIR/scripts/aix/export-coverage.pl" "$fixture/source" \
    "$fixture/outside.info" "$fixture/rejected.lcov" "$fixture/rejected.xml" \
    >"$fixture/rejection.log" 2>&1; then
    echo 'Failed coverage export test: an external source path was accepted' >&2
    exit 1
fi

echo 'TAP version 13'
echo '1..1'
echo 'ok 1 - coverage exports preserve LCOV, merge line/branch evidence, escape XML, and reject external sources'

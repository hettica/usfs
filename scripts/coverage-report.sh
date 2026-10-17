#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Render CMake/AIX gcov data as optional LCOV and HTML reports.
# Usage: USFS_SOURCE_DIR=... USFS_BINARY_DIR=... bash scripts/coverage-report.sh RUN

set -eu

run=${1:-}
case "$run" in
    ""|*[!A-Za-z0-9._-]*) echo "invalid coverage run name: $run" >&2; exit 2 ;;
esac
: "${USFS_SOURCE_DIR:?USFS_SOURCE_DIR is required}"
: "${USFS_BINARY_DIR:?USFS_BINARY_DIR is required}"

gcov --version | sed -n '1p' | grep -q '13.3.0' || {
    echo "gcov 13.3.0 is required on AIX" >&2
    exit 1
}
lcov --version
command -v genhtml >/dev/null || { echo "genhtml is required on AIX" >&2; exit 1; }

# LCOV 1.16 enables branches through its configuration; 2.x exposes a flag
# and diagnoses unmatched exclusion patterns with the "unused" error class.
lcov_options=(--rc lcov_branch_coverage=1 --ignore-errors source)
if lcov --help | grep -q -- '--branch-coverage'; then
    lcov_options=(--branch-coverage --ignore-errors source,unused)
fi

report="$USFS_BINARY_DIR/coverage/$run"
raw="$report/raw"
work="$report/work"
status=$(sed -n 's/^collection_status=//p' "$report/manifest.txt")
case "$status" in complete|partial) ;; *) echo "invalid coverage manifest" >&2; exit 1 ;; esac

recorded="$work/source-manifest.recorded"
current="$work/source-manifest.current"
rm -rf "$work" "$report/html" "$report/aix-unit-html" "$report/all-html"
mkdir -p "$work" "$report/html" "$report/aix-unit-html" "$report/all-html"
sed -n '/^source_manifest_begin$/,/^source_manifest_end$/p' \
    "$report/manifest.txt" >"$recorded"
{
    echo source_manifest_begin
    (cd "$USFS_SOURCE_DIR" && bash scripts/aix/source-manifest.sh)
    echo source_manifest_end
} >"$current"
diff "$recorded" "$current" >/dev/null || {
    echo "coverage source provenance does not match the instrumented build" >&2
    diff "$recorded" "$current" >&2 || true
    exit 1
}

capture()
{
    component=$1
    directory="$raw/$component"
    if [ -d "$directory" ] && find "$directory" -name '*.gcda' -print | grep -q .; then
        lcov "${lcov_options[@]}" --capture --gcov-tool gcov \
            --directory "$directory" \
            --output-file "$report/$component.unfiltered.info"
        # Tests execute production code, but their own counters must never enter
        # component, merged, HTML, or Sonar coverage metrics.
        lcov "${lcov_options[@]}" --extract "$report/$component.unfiltered.info" \
            "$USFS_SOURCE_DIR/src/*" "$USFS_SOURCE_DIR/examples/*" \
            --output-file "$work/$component.product.info"
        lcov "${lcov_options[@]}" --remove "$work/$component.product.info" \
            "${product_exclusions[@]}" --output-file "$report/$component.info"
        rm -f "$report/$component.unfiltered.info"
    else
        printf 'TN:\n' >"$report/$component.info"
    fi
}

# Keep these exclusions aligned with sonar.coverage.exclusions.
product_exclusions=(
    "$USFS_SOURCE_DIR/src/common/instrumentation/config_testing.c"
    "$USFS_SOURCE_DIR/src/kernel/instrumentation/coverage/*"
    "$USFS_SOURCE_DIR/src/kernel/instrumentation/testing/*"
)

capture kernel
capture client
capture cli
capture examples
capture aix-unit

trace_args=()
for component in kernel client cli examples; do
    if grep -q '^SF:' "$report/$component.info"; then
        trace_args+=(--add-tracefile "$report/$component.info")
    fi
done
[ "${#trace_args[@]}" -gt 0 ] || { echo "no product coverage records found" >&2; exit 1; }
lcov "${lcov_options[@]}" "${trace_args[@]}" --output-file "$report/combined.info"
genhtml "$report/combined.info" --branch-coverage --prefix "$USFS_SOURCE_DIR" \
    --title "USFS coverage: $run ($status)" --output-directory "$report/html"

if grep -q '^SF:' "$report/aix-unit.info"; then
    genhtml "$report/aix-unit.info" --branch-coverage --prefix "$USFS_SOURCE_DIR" \
        --title "USFS AIX unit coverage: $run ($status)" \
        --output-directory "$report/aix-unit-html"
    lcov "${lcov_options[@]}" --add-tracefile "$report/combined.info" \
        --add-tracefile "$report/aix-unit.info" \
        --output-file "$report/all.info"
else
    cp "$report/combined.info" "$report/all.info"
fi
genhtml "$report/all.info" --branch-coverage --prefix "$USFS_SOURCE_DIR" \
    --title "USFS all-suite coverage: $run ($status)" \
    --output-directory "$report/all-html"

printf '%s\n' "$status" >"$report/report-status.txt"
rm -rf "$work"
echo "Coverage reports: $report/html/index.html and $report/all-html/index.html"

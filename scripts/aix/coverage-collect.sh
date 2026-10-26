#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Quiesce and validate an AIX coverage run, take its final kernel snapshot,
#   unpack lifecycle exports, collect userspace profiles and build graphs, and
#   write the evidence manifest consumed by the AIX-side report renderer.
#
# Usage:
#   Run from the repository root on the AIX host:
#     bash scripts/aix/coverage-collect.sh nightly complete
#
#   Use "partial" instead of "complete" when salvaging a failed run. Complete
#   collection enforces the planned-suite, export, generation, and profile
#   expectations recorded by coverage-checkpoint.sh.

set -eu

run=${1:-}
collection_status=${2:-complete}
case "$run" in
    ""|*[!A-Za-z0-9._-]*)
        echo "invalid coverage run name: $run" >&2
        exit 2
        ;;
esac
case "$collection_status" in
    complete|partial) ;;
    *) echo "invalid coverage collection status: $collection_status" >&2; exit 2 ;;
esac

: "${USFS_BINARY_DIR:?USFS_BINARY_DIR is required}"
: "${USFS_SOURCE_DIR:?USFS_SOURCE_DIR is required}"
root="$USFS_BINARY_DIR/coverage"
report="$root/$run"
destination="$report/raw"
validation="$root/validate-$run"
kernel_data="$root/kernel-data/$run"
ledger="$root/user-data/$run/ledger.tsv"
kernel_snapshots=0
kernel_units=0

if mount | grep -i usfs >/dev/null 2>&1; then
    echo "coverage collection refused: USFS mount is active" >&2
    exit 1
fi
if ps -ef | grep -E '[u]sfs_(mirror|memfs)' >/dev/null 2>&1; then
    echo "coverage collection refused: USFS daemon is active" >&2
    exit 1
fi

rm -rf "$destination" "$report/graphs" "$validation"
mkdir -p "$destination" "$destination/client" \
    "$destination/examples" "$destination/aix-unit" "$destination/cli" \
    "$report/graphs/kernel" \
    "$report/graphs/client" "$report/graphs/examples" "$validation"

bash "$USFS_SOURCE_DIR/scripts/aix/coverage-checkpoint.sh" snapshot "$run" final
mkdir -p "$destination/kernel"
cp -R "$kernel_data"/[0-9][0-9][0-9][0-9]-* "$destination/kernel/"

# The resident proxy has its own gcov registry and survives implementation teardown.
mkdir -p "$destination/proxy" "$report/graphs/proxy"
/usr/sbin/usfs_coverage proxy-snapshot "$destination/proxy/final"
find "$USFS_BINARY_DIR/CMakeFiles/_usfs_proxy_coverage_objects.dir" -name '*.gcno' -print |
while IFS= read -r note; do
    cp "$note" "$report/graphs/proxy/"
    cp "$note" "$destination/proxy/final/"
done

actual_exports="$validation/actual-exports.txt"
actual_generations="$validation/actual-generations.txt"
: >"$actual_exports"
awk -F '\t' '{ print $3 }' "$kernel_data/checkpoints.tsv" \
    >"$actual_generations"
for container in "$kernel_data"/lifecycle/*.container; do
    [ -f "$container" ] || continue
    metadata="$container.meta"
    [ -f "$metadata" ] || {
        echo "missing lifecycle export metadata: $container" >&2
        exit 1
    }
    event=$(sed -n 's/^event=//p' "$metadata")
    generation=$(sed -n 's/^generation=//p' "$metadata")
    case "$event" in
        ""|*[!A-Za-z0-9._-]*)
            echo "invalid lifecycle export event: $metadata" >&2
            exit 1
            ;;
    esac
    case "$generation" in
        ""|*[!0-9]*)
            echo "invalid lifecycle export generation: $metadata" >&2
            exit 1
            ;;
    esac
    base=$(basename "$container" .container)
    snapshot="$destination/kernel/lifecycle-$base"
    /usr/sbin/usfs_coverage unpack "$container" "$snapshot"
    printf '%s\n' "$event" >>"$actual_exports"
    printf '%s\n' "$generation" >>"$actual_generations"
done

for unit_tree in "$USFS_BINARY_DIR"/CMakeFiles/_usfs_coverage_unit_*.dir; do
    [ -d "$unit_tree" ] || continue
    unit_name=$(basename "$unit_tree" .dir)
    cp -R "$unit_tree" "$destination/aix-unit/$unit_name"
done
if [ "$collection_status" = complete ]; then
    find "$destination/aix-unit" -name '*.gcda' -print | grep -q . || {
        echo "missing AIX userspace unit coverage profiles" >&2
        exit 1
    }
fi

# CLI executables run directly through ODM and administrative test commands.
# Their normal-exit profiles remain in the instrumented CMake object trees.
for program in methods cfgusfs ucfgusfs chusfs ctl; do
    object_tree="$USFS_BINARY_DIR/CMakeFiles/_usfs_coverage_$program.dir"
    [ -d "$object_tree" ] || {
        echo "Failed to collect CLI coverage: missing object tree $object_tree" >&2
        exit 1
    }
    cp -R "$object_tree" "$destination/cli/$program"
    if [ "$collection_status" = complete ]; then
        find "$destination/cli/$program" -name '*.gcda' -print | grep -q . || {
            echo "Failed to collect CLI coverage: missing profiles for $program" >&2
            exit 1
        }
    fi
done

for metadata in "$kernel_data"/lifecycle/*.meta; do
    [ -f "$metadata" ] || continue
    container=${metadata%.meta}
    [ -f "$container" ] || {
        echo "lifecycle metadata has no container: $metadata" >&2
        exit 1
    }
done

for snapshot in "$destination"/kernel/*; do
    [ -d "$snapshot" ] || {
        echo "missing kernel coverage snapshots" >&2
        exit 1
    }
    snapshot_units=0
    for note in $(find "$USFS_BINARY_DIR/CMakeFiles/_usfs_kernel_coverage_core.dir" \
        "$USFS_BINARY_DIR/CMakeFiles/_usfs_kernel_coverage_instrumentation.dir" \
        -name '*.gcno' -print); do
        base=$(basename "$note" .gcno)
        data="$snapshot/$base.gcda"
        [ -f "$data" ] || {
            echo "missing kernel profile in $(basename "$snapshot"): $base.gcda" >&2
            exit 1
        }
        cp "$note" "$report/graphs/kernel/"
        cp "$note" "$snapshot/"
        cp "$note" "$data" "$validation/"
        (cd "$validation" && gcov -b -c "$base.gcno" >/dev/null)
        rm -f "$validation/$base.gcno" "$validation/$base.gcda"
        snapshot_units=$((snapshot_units + 1))
    done
    if [ "$kernel_units" -eq 0 ]; then
        kernel_units=$snapshot_units
    elif [ "$snapshot_units" -ne "$kernel_units" ]; then
        echo "kernel unit count differs between snapshots" >&2
        exit 1
    fi
    kernel_snapshots=$((kernel_snapshots + 1))
    echo "validated kernel coverage snapshot: $(basename "$snapshot") ($snapshot_units units)"
done

if [ "$collection_status" = complete ]; then
    [ -f "$ledger" ] || {
        echo "missing coverage suite ledger" >&2
        exit 1
    }
    planned="$validation/planned.txt"
    expected_exports="$validation/expected-exports.txt"
    awk -F '\t' '$1 == "planned" { print $2 }' "$ledger" | sort >"$planned"
    [ -s "$planned" ] || {
        echo "coverage suite ledger has no plan" >&2
        exit 1
    }
    [ -z "$(uniq -d "$planned")" ] || {
        echo "coverage suite ledger contains duplicate plans" >&2
        exit 1
    }
    awk -F '\t' '$1 == "started" || $1 == "completed" || $1 == "failed" { print $2 }' \
        "$ledger" | sort -u |
    while IFS= read -r suite; do
        grep -qx "$suite" "$planned" || {
            echo "coverage suite event was not planned: $suite" >&2
            exit 1
        }
    done
    while IFS= read -r suite; do
        [ "$(awk -F '\t' -v s="$suite" '$1 == "started" && $2 == s { n++ } END { print n+0 }' "$ledger")" -eq 1 ] &&
        [ "$(awk -F '\t' -v s="$suite" '$1 == "completed" && $2 == s { n++ } END { print n+0 }' "$ledger")" -eq 1 ] &&
        [ "$(awk -F '\t' -v s="$suite" '$1 == "failed" && $2 == s { n++ } END { print n+0 }' "$ledger")" -eq 0 ] || {
            echo "coverage suite did not complete exactly once: $suite" >&2
            exit 1
        }
    done <"$planned"
    awk -F '\t' '$1 == "expect-export" { print $2 }' "$ledger" |
        sort >"$expected_exports"
    sort "$actual_exports" >"$actual_exports.sorted"
    if ! diff "$expected_exports" "$actual_exports.sorted" >/dev/null; then
        echo "lifecycle coverage exports do not match the plan" >&2
        diff "$expected_exports" "$actual_exports.sorted" >&2 || true
        exit 1
    fi
    sort -n -u "$actual_generations" >"$actual_generations.sorted"
    expected_generation=1
    while IFS= read -r generation; do
        [ "$generation" -eq "$expected_generation" ] || {
            echo "missing kernel coverage generation: $expected_generation" >&2
            exit 1
        }
        expected_generation=$((expected_generation + 1))
    done <"$actual_generations.sorted"
fi

find "$USFS_BINARY_DIR/CMakeFiles/_usfs_client_coverage.dir" -name '*.gcno' -print |
while IFS= read -r note; do
    cp "$note" "$report/graphs/client/"
done
for tree in "$USFS_BINARY_DIR"/CMakeFiles/_usfs_coverage_mirror.dir \
    "$USFS_BINARY_DIR"/CMakeFiles/_usfs_coverage_memfs.dir; do
    find "$tree" -name '*.gcno' -print | while IFS= read -r note; do
        cp "$note" "$report/graphs/examples/"
    done
done

# Units and integration tests can launch instrumented code without the daemon
# wrapper. Preserve the normal process-exit profiles from those build trees too.
collect_build_profiles()
{
    local component=$1
    local object_directory=$2
    local profile_destination="$destination/$component/build-tree"

    find "$object_directory" -name '*.gcda' -print |
    while IFS= read -r data; do
        base=$(basename "$data" .gcda)
        note="$report/graphs/$component/$base.gcno"
        [ -f "$note" ] || { echo "missing graph for $data" >&2; exit 1; }
        cp "$data" "$note" "$validation/"
        if ! (cd "$validation" && gcov -b -c "$base.gcno" >/dev/null); then
            # Direct test launches share this build-tree path. An intentionally
            # killed daemon can leave a partial profile; required graceful
            # profiles are checked separately in per-process directories.
            echo "skipping invalid optional build-tree profile: $data" >&2
            rm -f "$validation/$base.gcno" "$validation/$base.gcda"
            continue
        fi
        mkdir -p "$profile_destination"
        cp "$data" "$note" "$profile_destination/"
        rm -f "$validation/$base.gcno" "$validation/$base.gcda"
    done
}

collect_build_profiles client "$USFS_BINARY_DIR/CMakeFiles/_usfs_client_coverage.dir"
collect_build_profiles examples "$USFS_BINARY_DIR/CMakeFiles/_usfs_coverage_mirror.dir"
collect_build_profiles examples "$USFS_BINARY_DIR/CMakeFiles/_usfs_coverage_memfs.dir"

if [ -d "$root/user-data/$run" ]; then
    for process_dir in "$root/user-data/$run"/[0-9]*; do
        [ -d "$process_dir" ] || continue
        instance=$(basename "$process_dir")
        find "$process_dir" -name '*.gcda' -print | while IFS= read -r data; do
            base=$(basename "$data" .gcda)
            case "$base" in
                usfs_client.c|usfs_validate.c) component=client ;;
                mirror.c|memfs.c) component=examples ;;
                *) echo "unexpected user profile: $data" >&2; exit 1 ;;
            esac
            note=$(find "$report/graphs/$component" -name "$base.gcno" -print | sed -n '1p')
            [ -f "$note" ] || { echo "missing graph for $data" >&2; exit 1; }
            mkdir -p "$destination/$component/$instance"
            cp "$data" "$destination/$component/$instance/"
            cp "$note" "$destination/$component/$instance/"
            cp "$note" "$data" "$validation/"
            (cd "$validation" && gcov -b -c "$base.gcno" >/dev/null)
            rm -f "$validation/$base.gcno" "$validation/$base.gcda"
        done
    done
fi

if [ "$collection_status" = complete ] && [ -f "$ledger" ]; then
    awk -F '\t' '$1 == "expect-profile" { print $2 "\t" $3 }' "$ledger" |
    while IFS="$(printf '\t')" read -r pid workload; do
        find "$destination/client/$pid" -name 'usfs_client.c.gcda' -print | grep -q . &&
        find "$destination/client/$pid" -name 'usfs_validate.c.gcda' -print | grep -q . &&
        find "$destination/examples/$pid" -name "$workload.c.gcda" -print | grep -q . || {
            echo "missing graceful user coverage profile: $pid $workload" >&2
            exit 1
        }
    done
fi

{
    echo "run=$run"
    echo "collection_status=$collection_status"
    [ -f "$root/build-manifest.txt" ] && cat "$root/build-manifest.txt"
    if [ -f "$ledger" ]; then
        echo "ledger_begin"
        cat "$ledger"
        echo "ledger_end"
    else
        echo "ledger=manual-or-unrecorded"
    fi
    echo "aix=$(uname -a)"
    echo "kernel_extension_status_begin"
    /usr/sbin/usfsctl kexts
    echo "kernel_extension_status_end"
    echo "proxy_binary_sha256=$(perl "$USFS_SOURCE_DIR/scripts/aix/sha256.pl" "$USFS_BINARY_DIR/coverage/bin/usfs_proxy.kext" | awk '{ print $1 }')"
    echo "implementation_binary_sha256=$(perl "$USFS_SOURCE_DIR/scripts/aix/sha256.pl" "$USFS_BINARY_DIR/coverage/bin/usfs.kext" | awk '{ print $1 }')"
    echo "gcc=$(gcc -dumpfullversion)"
    echo "gcov=$(gcov --version | sed -n '1p')"
    echo "kernel_snapshots=$kernel_snapshots"
    echo "kernel_units_per_snapshot=$kernel_units"
    echo "kernel_checkpoints_begin"
    cat "$kernel_data/checkpoints.tsv"
    for metadata in "$kernel_data"/lifecycle/*.meta; do
        [ -f "$metadata" ] || continue
        echo "lifecycle_export=$(basename "$metadata" .container.meta)"
        sed 's/^/  /' "$metadata"
    done
    echo "kernel_checkpoints_end"
    echo "client_profiles=$(find "$destination/client" -name '*.gcda' | wc -l)"
    echo "example_profiles=$(find "$destination/examples" -name '*.gcda' | wc -l)"
    echo "cli_profiles=$(find "$destination/cli" -name '*.gcda' | wc -l)"
    echo "aix_unit_profiles=$(find "$destination/aix-unit" -name '*.gcda' | wc -l)"
    echo "proxy_profiles=$(find "$destination/proxy" -name '*.gcda' | wc -l)"
    if [ -f "$ledger" ] &&
       grep -E 'planned[	]e2e($|-)' "$ledger" >/dev/null; then
        echo "sigkill_user_profile_loss_expected=yes"
    else
        echo "sigkill_user_profile_loss_expected=no"
    fi
    echo "collected_utc=$(TZ=UTC date '+%Y-%m-%dT%H:%M:%SZ')"
} > "$report/manifest.txt"

rm -rf "$validation"
echo "coverage artifacts validated and staged at $report"

#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Run read-only usfs_mirror daemons, compare mounted trees with their source,
#   reject every mutation, exercise concurrent access, and verify clean
#   shutdown.
#
# Each daemon mirrors a source directory and mounts it at
# /mnt/<pid>/<basename of source>.
#
# Usage:
#   Run through `make test-e2e` in the AIX CMake binary directory.
#   On a prepared AIX VM:
#     MIRROR_PHASES=structure,content bash tests/e2e/mirror.sh
#
# Every step prints a timestamped progress line, and every wait is bounded by a
# deadline that reports TIMEOUT rather than blocking forever, so a stuck run is
# always distinguishable from a slow one. Progress goes to stdout and to
# $LOGFILE.
#
# Environment knobs:
#   MIRROR_PHASES  comma-separated phases to run, or "all"   (default all)
#                  structure content dotdot mutations bigdir concurrent
#                  identity exec real shutdown
#   MIRROR_SRC     real subtree used by the "real" phase     (default /usr/include)
#   N_ENTRIES      max paths compared in the "real" phase    (default 400)
#   N_DAEMONS      concurrent mirrors in the shutdown phase  (default 2)
#   ITERATIONS     iterations of the shutdown phase          (default 2)
#   N_READERS      concurrent readers per mount              (default 2)
#   MOUNT_WAIT / READ_WAIT / STOP_WAIT   seconds             (90 / 240 / 60)
#   FIXTURE        fixture location            (default /tmp/usfs_fixture)
#   LOGFILE        progress log                (default /tmp/usfs_mirror.log)
#
# The VM is slow; defaults are deliberately small. For a soak run over the
# whole of /usr:  MIRROR_SRC=/usr N_ENTRIES=100000 ITERATIONS=1

set -u

# This is an unattended gate; never let child utilities prompt on the SSH TTY.
exec </dev/null

MIRROR_PHASES=${MIRROR_PHASES:-all}
MIRROR_SRC=${MIRROR_SRC:-/usr/include}
N_ENTRIES=${N_ENTRIES:-400}
N_DAEMONS=${N_DAEMONS:-2}
ITERATIONS=${ITERATIONS:-2}
N_READERS=${N_READERS:-2}
MOUNT_WAIT=${MOUNT_WAIT:-90}
READ_WAIT=${READ_WAIT:-240}
STOP_WAIT=${STOP_WAIT:-60}
FIXTURE=${FIXTURE:-/tmp/usfs_fixture}
LOGFILE=${LOGFILE:-/tmp/usfs_mirror.log}

DAEMON=/usr/sbin/usfs_mirror
DAEMON_RE='[u]sfs_mirror'

coverage_expect_profile()
{
    return 0
}

START=$(date +%s)
failures=0

: > "$LOGFILE"

say()
{
    now=$(date +%s)
    printf '[T+%04d] %s\n' "$((now - START))" "$*" | tee -a "$LOGFILE"
}

fail()
{
    failures=$((failures + 1))
    say "FAIL: $*"
}

snapshot()
{
    d=$(ps -ef | grep -cE "$DAEMON_RE")
    m=$(mount 2>/dev/null | awk '$3 == "usfs" { print $2 }' | wc -l | tr -d ' ')
    say "STATE daemons=$d mounts=$m ($*)"
}

phase_enabled()
{
    case "$MIRROR_PHASES" in
        all) return 0 ;;
        *"$1"*) return 0 ;;
        *) return 1 ;;
    esac
}

is_mounted()
{
    mount 2>/dev/null | awk '$3 == "usfs" { print $2 }' | grep -qx "$1"
}

# Waits until $1 is mounted or $2 seconds elapse. Non-zero on timeout.
wait_mounted()
{
    _deadline=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$_deadline" ]; do
        if is_mounted "$1"; then
            return 0
        fi
        sleep 2
    done
    return 1
}

# Waits until process $1 exits or $2 seconds elapse.
wait_gone()
{
    _deadline=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$_deadline" ]; do
        if ! ps -p "$1" >/dev/null 2>&1; then
            return 0
        fi
        sleep 2
    done
    return 1
}

# Starts a mirror of $1. Echoes "<pid> <mountpoint>" on success.
start_mirror()
{
    _src=$1
    _base=$(basename "$_src")

    $DAEMON --source="$_src" </dev/null >>"$LOGFILE" 2>&1 &
    _pid=$!
    _mnt=/mnt/$_pid/$_base

    if wait_mounted "$_mnt" "$MOUNT_WAIT"; then
        echo "$_pid $_mnt"
        return 0
    fi

    return 1
}

stop_mirror()
{
    coverage_expect_profile "$1" || fail "record graceful profile $1"
    kill -TERM "$1" 2>/dev/null
    wait_gone "$1" "$STOP_WAIT"
    rmdir "$2" "/mnt/$1" 2>/dev/null
}

# ---------------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------------

build_fixture()
{
    rm -rf "$FIXTURE"
    mkdir -p "$FIXTURE/a/b/c/d/e/f" || return 1

    echo "hello from the fixture" > "$FIXTURE/small.txt"

    # Larger than USFS_MAX_DATA (64 KiB) so reads are split into chunks.
    i=0
    : > "$FIXTURE/big.bin"
    while [ $i -lt 256 ]; do
        dd if=/dev/zero bs=1024 count=1 2>/dev/null | tr '\0' 'x' >> "$FIXTURE/big.bin"
        i=$((i + 1))
    done

    echo "deep" > "$FIXTURE/a/b/c/d/e/f/deep.txt"

    ln -s small.txt "$FIXTURE/link_to_file"
    ln -s a "$FIXTURE/link_to_dir"
    ln -s nowhere "$FIXTURE/dangling"

    # Enough entries that a listing spans several readdir windows.
    mkdir -p "$FIXTURE/many"
    i=0
    while [ $i -lt 300 ]; do
        echo "$i" > "$FIXTURE/many/file_$i"
        i=$((i + 1))
    done

    return 0
}

# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------

phase_structure()
{
    say "STEP structure: comparing the tree against the source"

    # No -follow: absolute symlinks in the source resolve outside the mirror.
    ( cd "$FIXTURE" && find . | sort ) > /tmp/usfs_src.list 2>/dev/null
    ( cd "$1" && find . | sort ) > /tmp/usfs_mnt.list 2>/dev/null

    if diff /tmp/usfs_src.list /tmp/usfs_mnt.list > /tmp/usfs_diff.txt 2>&1; then
        say "OK   structure matches ($(wc -l < /tmp/usfs_src.list | tr -d ' ') paths)"
    else
        fail "structure differs from the source"
        head -20 /tmp/usfs_diff.txt | tee -a "$LOGFILE"
    fi

    say "STEP structure: symlink targets"
    for link in link_to_file link_to_dir dangling; do
        want=$(ls -l "$FIXTURE/$link" 2>/dev/null | sed 's/.*-> //')
        got=$(ls -l "$1/$link" 2>/dev/null | sed 's/.*-> //')

        if [ -n "$got" ] && [ "$want" = "$got" ]; then
            say "OK   $link -> $got"
        else
            fail "$link target is '$got', expected '$want'"
        fi
    done
}

phase_content()
{
    say "STEP content: comparing file contents"

    for f in small.txt big.bin a/b/c/d/e/f/deep.txt; do
        # Piped through cat so nothing tries to mmap the mirrored file.
        want=$(cat "$FIXTURE/$f" 2>/dev/null | cksum)
        got=$(cat "$1/$f" 2>/dev/null | cksum)

        if [ "$want" = "$got" ]; then
            say "OK   $f matches ($want)"
        else
            fail "$f differs: mirror '$got' vs source '$want'"
        fi
    done
}

phase_dotdot()
{
    say "STEP dotdot: parent traversal"

    got=$(cd "$1/a/b/c/d/e/f" 2>/dev/null && cd ../../.. && pwd)
    if [ "$got" = "$1/a/b/c" ]; then
        say "OK   cd ../../.. resolves to $got"
    else
        fail "cd ../../.. gave '$got', expected '$1/a/b/c'"
    fi

    # Leaving the mount root must land on the covered directory, not loop back
    # into the mount.
    got=$(cd "$1" 2>/dev/null && cd .. && pwd)
    want=$(dirname "$1")
    if [ "$got" = "$want" ]; then
        say "OK   cd .. from the mount root resolves to $got"
    else
        fail "cd .. from the mount root gave '$got', expected '$want'"
    fi
}

phase_mutations()
{
    say "STEP mutations: every write must be refused"

    # A mutation must fail. AIX renders EROFS as "The file system has read
    # permission only."; other wordings are tolerated, but "not implemented"
    # means ENOSYS leaked through where EROFS was intended.
    _check()
    {
        _what=$1
        shift

        if _err=$("$@" 2>&1); then
            fail "$_what unexpectedly succeeded"
            return
        fi

        if echo "$_err" | grep -qi "not implemented"; then
            fail "$_what refused with ENOSYS, expected EROFS: $_err"
        elif echo "$_err" | grep -qiE "read permission only|read-only"; then
            say "OK   $_what refused as read-only"
        else
            say "OK   $_what refused ($(echo "$_err" | head -1))"
        fi
    }

    _check "touch new file"   touch "$1/newfile"
    _check "mkdir"            mkdir "$1/newdir"
    _check "rm existing file" rm -f "$1/small.txt"
    _check "symlink"          ln -s target "$1/newlink"
    _check "rename"           mv "$1/small.txt" "$1/renamed.txt"
    _check "chmod"            chmod 777 "$1/small.txt"

    _err=$( (echo data > "$1/small.txt") 2>&1 )
    if [ $? -eq 0 ]; then
        fail "redirect into an existing file unexpectedly succeeded"
    else
        say "OK   redirect refused: $_err"
    fi

    # The source must be untouched by all of the above.
    if [ -e "$FIXTURE/newfile" ] || [ -d "$FIXTURE/newdir" ] ||
       [ ! -f "$FIXTURE/small.txt" ] || [ -e "$FIXTURE/renamed.txt" ]; then
        fail "the source tree was modified through the mirror"
    else
        say "OK   source tree is unchanged"
    fi
}

phase_bigdir()
{
    say "STEP bigdir: listing a directory with many entries"

    want=$(ls "$FIXTURE/many" | wc -l | tr -d ' ')

    t0=$(date +%s)
    got=$(ls "$1/many" | wc -l | tr -d ' ')
    t1=$(date +%s)

    if [ "$want" = "$got" ]; then
        say "OK   listed $got entries in $((t1 - t0))s"
    else
        fail "listed $got entries, expected $want"
    fi
}

phase_identity()
{
    say "STEP identity: alternating directory readers, close, seek, dup, and fork"

    if /usr/sbin/usfs_io_probe serial-two-open "$1/many" >>"$LOGFILE" 2>&1; then
        say "OK   serial two-open reproducer returns every entry"
    else
        fail "serial two-open reproducer lost directory entries"
    fi

    if /usr/sbin/usfs_io_probe directory-readers "$1/many" >>"$LOGFILE" 2>&1; then
        say "OK   directory readers retain their own cursor state"
    else
        fail "directory reader identity or continuation failed"
    fi

    say "STEP identity: reading objects after source rename, removal, and replacement"
    if /usr/sbin/usfs_io_probe mirror-detached-objects "$FIXTURE" "$1" >>"$LOGFILE" 2>&1; then
        say "OK   held mirror objects survive source name changes"
    else
        fail "mirror selected a replacement or lost a detached object"
    fi
}

phase_concurrent()
{
    say "STEP concurrent: $N_READERS readers against one mount"

    reader_pids=""
    i=1
    while [ "$i" -le "$N_READERS" ]; do
        (
            n=0
            while [ $n -lt 3 ]; do
                cat "$1/small.txt" >/dev/null 2>&1 || exit 1
                cat "$1/big.bin"   >/dev/null 2>&1 || exit 1
                ls "$1/many"       >/dev/null 2>&1 || exit 1
                n=$((n + 1))
            done
        ) &
        reader_pids="$reader_pids $!"
        i=$((i + 1))
    done

    deadline=$(( $(date +%s) + READ_WAIT ))
    for rp in $reader_pids; do
        while ps -p "$rp" >/dev/null 2>&1 && [ "$(date +%s)" -lt "$deadline" ]; do
            sleep 2
        done
        if ps -p "$rp" >/dev/null 2>&1; then
            fail "TIMEOUT reader $rp still running after ${READ_WAIT}s"
            kill -9 "$rp" 2>/dev/null
        elif ! wait "$rp"; then
            fail "reader $rp reported a read error"
        fi
    done

    say "OK   concurrent readers finished"
}

phase_statfs()
{
    say "STEP statfs: df should describe the source, not a placeholder"

    # Field 2 of df -k is the total size in KB. The mirror reports the numbers
    # of the file system the source lives on.
    want=$(df -k "$FIXTURE" 2>/dev/null | tail -1 | awk '{ print $2 }')
    got=$(df -k "$1" 2>/dev/null | tail -1 | awk '{ print $2 }')

    say "df source=$want mirror=$got"

    if [ -z "$got" ]; then
        fail "df on the mirror produced nothing"
    elif [ "$want" = "$got" ]; then
        say "OK   df matches the source file system"
    else
        fail "df reports $got, expected $want"
    fi
}

phase_exec()
{
    # Two cases that need different things from the file system:
    #   a shell script  - the interpreter only read()s it
    #   a real binary   - the loader maps it, which needs paging support
    # Running both shows whether paging is genuinely required.
    say "STEP exec: running a shell script from the mirror"

    printf '#!/bin/sh\necho script_ran\n' > "$FIXTURE/script.sh"
    chmod 555 "$FIXTURE/script.sh"

    out=$("$1/script.sh" 2>&1)
    if [ "$out" = "script_ran" ]; then
        say "OK   shell script ran from the mirror"
    else
        say "NOTE shell script did not run: $out"
    fi

    say "STEP exec: running a binary from the mirror"

    bin=/usr/bin/true
    [ -x "$bin" ] || bin=/bin/true
    if [ ! -x "$bin" ]; then
        say "SKIP no true(1) binary to copy"
        return
    fi

    cp "$bin" "$FIXTURE/true_copy" 2>/dev/null
    chmod 555 "$FIXTURE/true_copy" 2>/dev/null

    # Sanity: the copy must be runnable from the real file system, so a
    # failure below is attributable to the mirror and not to the copy.
    if ! "$FIXTURE/true_copy" 2>/dev/null; then
        say "SKIP the copied binary does not run from the source either"
        return
    fi

    out=$("$1/true_copy" 2>&1)
    rc=$?

    if [ $rc -eq 0 ]; then
        say "OK   executed a small mirrored binary ($(ls -l "$bin" | awk '{print $5}') bytes)"
    else
        fail "cannot execute a small mirrored binary (rc=$rc): $out"
    fi

    # A binary large enough to span many pages and several read chunks, which
    # a small one would not exercise.
    say "STEP exec: running a large mirrored binary"

    big=/usr/bin/ksh
    if [ ! -x "$big" ]; then
        say "SKIP no large binary available"
        return
    fi

    cp "$big" "$FIXTURE/big_copy" 2>/dev/null
    chmod 555 "$FIXTURE/big_copy" 2>/dev/null

    out=$("$1/big_copy" -c 'echo big_ran' 2>&1)
    if [ "$out" = "big_ran" ]; then
        say "OK   executed a $(ls -l "$big" | awk '{print $5}')-byte mirrored binary"
    else
        fail "cannot execute a large mirrored binary: $out"
    fi
}

phase_real()
{
    say "STEP real: mirroring $MIRROR_SRC"

    info=$(start_mirror "$MIRROR_SRC")
    if [ -z "$info" ]; then
        fail "TIMEOUT mirror of $MIRROR_SRC never mounted"
        return
    fi

    pid=${info% *}
    mnt=${info#* }
    say "OK   mounted $mnt (pid $pid)"

    ( cd "$MIRROR_SRC" && find . | sort | head -n "$N_ENTRIES" ) > /tmp/usfs_rsrc.list 2>/dev/null
    ( cd "$mnt" && find . | sort | head -n "$N_ENTRIES" ) > /tmp/usfs_rmnt.list 2>/dev/null

    if diff /tmp/usfs_rsrc.list /tmp/usfs_rmnt.list > /tmp/usfs_rdiff.txt 2>&1; then
        say "OK   first $N_ENTRIES paths match"
    else
        fail "$MIRROR_SRC structure differs"
        head -20 /tmp/usfs_rdiff.txt | tee -a "$LOGFILE"
    fi

    sample=$( (cd "$MIRROR_SRC" && find . -type f | head -3) 2>/dev/null )
    for rel in $sample; do
        want=$(cat "$MIRROR_SRC/$rel" 2>/dev/null | cksum)
        got=$(cat "$mnt/$rel" 2>/dev/null | cksum)
        if [ "$want" = "$got" ]; then
            say "OK   $rel matches"
        else
            fail "$rel differs"
        fi
    done

    stop_mirror "$pid" "$mnt"
}

phase_shutdown()
{
    iter=1
    while [ "$iter" -le "$ITERATIONS" ]; do
        say "=== shutdown iteration $iter/$ITERATIONS"

        pids=""
        mnts=""
        i=1
        while [ "$i" -le "$N_DAEMONS" ]; do
            info=$(start_mirror "$FIXTURE")
            if [ -z "$info" ]; then
                fail "TIMEOUT daemon $i never mounted"
            else
                pids="$pids ${info% *}"
                mnts="$mnts ${info#* }"
                say "OK   daemon $i mounted ${info#* }"
            fi
            i=$((i + 1))
        done

        snapshot "after mount"

        k=1
        for pid in $pids; do
            mnt=$(echo "$mnts" | awk -v n="$k" '{ print $n }')

            if [ $((k % 2)) -eq 1 ]; then
                say "STEP kill -9 daemon $pid"
                kill -9 "$pid" 2>/dev/null
                wait_gone "$pid" "$STOP_WAIT" || fail "daemon $pid survived SIGKILL"

                if is_mounted "$mnt"; then
                    if cat "$mnt/small.txt" >/dev/null 2>&1; then
                        fail "$mnt still readable after kill -9"
                    else
                        say "OK   $mnt reports an error as expected"
                    fi

                    if umount "$mnt" >>"$LOGFILE" 2>&1; then
                        say "OK   unmounted $mnt"
                    else
                        fail "cannot umount $mnt after kill -9"
                    fi
                fi
                rmdir "$mnt" "/mnt/$pid" 2>/dev/null
            else
                say "STEP kill -TERM daemon $pid"
                coverage_expect_profile "$pid" ||
                    fail "record graceful profile $pid"
                kill -TERM "$pid" 2>/dev/null
                if wait_gone "$pid" "$STOP_WAIT" && ! is_mounted "$mnt"; then
                    say "OK   $mnt self-unmounted"
                else
                    fail "$mnt still mounted after SIGTERM"
                    umount "$mnt" >>"$LOGFILE" 2>&1
                fi
                rmdir "$mnt" "/mnt/$pid" 2>/dev/null
            fi

            k=$((k + 1))
        done

        snapshot "after cleanup"
        iter=$((iter + 1))
    done
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

say "=== USFS mirror test (phases: $MIRROR_PHASES)"
say "STEP device setup"

if ! bash "$(dirname "$0")/../../scripts/aix/setup.sh" >>"$LOGFILE" 2>&1; then
    say "FATAL device setup failed (see $LOGFILE)"
    exit 1
fi

say "OK   device ready"

say "STEP building fixture at $FIXTURE"
if ! build_fixture; then
    say "FATAL cannot build the fixture"
    exit 1
fi
say "OK   fixture ready"

snapshot "before start"

# Phases that share one long-lived mirror of the fixture.
if phase_enabled structure || phase_enabled content || phase_enabled dotdot ||
   phase_enabled mutations || phase_enabled bigdir || phase_enabled statfs ||
   phase_enabled concurrent || phase_enabled identity || phase_enabled exec; then

    say "STEP starting mirror of the fixture"
    info=$(start_mirror "$FIXTURE")

    if [ -z "$info" ]; then
        fail "TIMEOUT fixture mirror never mounted"
    else
        fpid=${info% *}
        fmnt=${info#* }
        say "OK   mounted $fmnt (pid $fpid)"

        phase_enabled structure  && phase_structure  "$fmnt"
        phase_enabled content    && phase_content    "$fmnt"
        phase_enabled dotdot     && phase_dotdot     "$fmnt"
        phase_enabled mutations  && phase_mutations  "$fmnt"
        phase_enabled bigdir     && phase_bigdir     "$fmnt"
        phase_enabled statfs     && phase_statfs     "$fmnt"
        phase_enabled concurrent && phase_concurrent "$fmnt"
        phase_enabled identity   && phase_identity   "$fmnt"
        phase_enabled exec       && phase_exec       "$fmnt"

        say "STEP stopping the fixture mirror"
        stop_mirror "$fpid" "$fmnt"

        if is_mounted "$fmnt"; then
            fail "$fmnt still mounted after SIGTERM"
            umount "$fmnt" >>"$LOGFILE" 2>&1
        else
            say "OK   $fmnt self-unmounted"
        fi
    fi
fi

phase_enabled real     && phase_real
phase_enabled shutdown && phase_shutdown

say "=== finished"
snapshot "final"

rm -rf "$FIXTURE"

leftover=$(mount 2>/dev/null | awk '$3 == "usfs" { print $2 }' | wc -l | tr -d ' ')
if [ "$leftover" -ne 0 ]; then
    fail "$leftover usfs mount(s) left over"
    mount 2>/dev/null | grep usfs | tee -a "$LOGFILE"
fi

if [ "$failures" -eq 0 ]; then
    say "ALL TESTS PASSED"
    exit 0
fi

say "$failures FAILURE(S)"
exit 1

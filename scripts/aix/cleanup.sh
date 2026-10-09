#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Remove all USFS runtime state from the VM: stop every USFS demo daemon
#   (usfs_mirror, usfs_memfs, scenario daemons, and the
#   multithreaded test daemon) and unmount
#   every USFS file system. Report what remains so cleanup is verifiable.
#
# Safe to run at any time; it is idempotent and touches nothing else.
#
# Usage:
#   Run from the repository root on the AIX VM:
#     bash scripts/aix/cleanup.sh

# Bracketed first character keeps the grep from matching itself; -E because the
# alternation needs extended syntax (plain grep would take "(" and "|"
# literally and silently match nothing, reporting a clean system that is not).
DAEMON_RE='[u]sfs_(mirror|memfs|scenario_daemon|mt_test_daemon)'

daemon_pids()
{
    ps -ef | grep -E "$DAEMON_RE" | awk '{ print $2 }'
}

report_state()
{
    daemons=$(ps -ef | grep -cE "$DAEMON_RE")
    mounts=$(mount 2>/dev/null | awk '$3 == "usfs" { print $2 }' | wc -l | tr -d ' ')
    echo "state: daemons=$daemons mounts=$mounts"
}

echo "== before"
report_state
ps -ef | grep -E "$DAEMON_RE"
mount 2>/dev/null | awk '$3 == "usfs" { print "  mounted: " $2 }'

echo
echo "== stopping daemons"
for pid in $(daemon_pids); do
    echo "  kill -TERM $pid"
    kill -TERM "$pid" 2>/dev/null
done

sleep 5

for pid in $(daemon_pids); do
    echo "  kill -9 $pid"
    kill -9 "$pid" 2>/dev/null
done

sleep 3

echo
echo "== unmounting"
for mnt in $(mount 2>/dev/null | awk '$3 == "usfs" { print $2 }'); do
    if umount "$mnt" 2>/dev/null; then
        echo "  unmounted $mnt"
    elif umount -f "$mnt" 2>/dev/null; then
        echo "  force unmounted $mnt"
    else
        echo "  STILL BUSY: $mnt"
    fi
    rmdir "$mnt" 2>/dev/null
    rmdir "$(dirname "$mnt")" 2>/dev/null
done

echo
echo "== after"
report_state
mount 2>/dev/null | awk '$3 == "usfs" { print "  still mounted: " $2 }'

remaining=$(mount 2>/dev/null | awk '$3 == "usfs" { print $2 }' | wc -l | tr -d ' ')
if [ "$remaining" -ne 0 ]; then
    echo "CLEANUP INCOMPLETE"
    exit 1
fi

echo "CLEANUP OK"
exit 0

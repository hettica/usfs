#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify the installed formatter and supported native AIX trace lifecycle.
#
# Usage:
#   On a testing AIX deployment: bash tests/integration/aix/contract/system_trace.sh

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-system-trace.$$
RAW=$BASE/full.trc
REPORT=$RAW.txt
EXPLICIT=$BASE/explicit.txt
OTHER=$BASE/administrator.trc

cleanup()
{
    /usr/sbin/usfsctl trace status >/dev/null 2>&1 &&
        /usr/sbin/usfsctl trace stop >/dev/null 2>&1
    rm -rf "$BASE"
}
trap cleanup 0 1 2 15
mkdir -p "$BASE"

tap_plan 13

/usr/bin/trcrpt -c -t /usr/lib/ras/usfs.trcfmt >/dev/null 2>&1
tap_ok "$?" "standalone USFS trace formatter is valid"

/usr/bin/trcrpt -c -t /etc/trcfmt >/dev/null 2>&1
tap_ok "$?" "registered AIX formatter database is valid"

/usr/sbin/usfsctl trace start --profile full --output "$RAW" \
    >"$BASE/start.out" 2>"$BASE/start.err"
tap_ok "$?" "full native trace starts through usfsctl"

/usr/sbin/usfsctl trace status >"$BASE/status.out" 2>&1
tap_ok $(grep -q "profile=full raw_output_path=$RAW" "$BASE/status.out"; echo $?) \
    "trace ownership status identifies the session"

tap_ok $(awk 'NF == 7 && $5 > 1 && $6 > 0 { found=1 } END { exit !found }' \
         /var/adm/ras/usfs.trace.state; echo $?) \
    "ownership record binds the native daemon PID and start time"

cp /var/adm/ras/usfs.trace.state "$BASE/state.saved"
awk '{$5=1; print}' "$BASE/state.saved" >/var/adm/ras/usfs.trace.state
/usr/sbin/usfsctl trace pause >/dev/null 2>&1
stale_rc=$?
cp "$BASE/state.saved" /var/adm/ras/usfs.trace.state
tap_ok $(test "$stale_rc" -eq 2; echo $?) \
    "stale daemon ownership is refused without touching channel 0"

/usr/sbin/usfsctl trace pause >/dev/null 2>&1 &&
    /usr/sbin/usfsctl trace resume >/dev/null 2>&1
tap_ok "$?" "owned trace pauses and resumes"

/usr/sbin/usfs_testctl selftest 3 >/dev/null 2>&1
/usr/sbin/usfsctl >/dev/null 2>&1
/usr/sbin/usfs_probe >/dev/null 2>&1
/usr/sbin/usfsctl trace stop >"$BASE/stop.out" 2>"$BASE/stop.err"
tap_ok $(test -s "$RAW" -a -s "$REPORT"; echo $?) \
    "stop retains raw and human-readable artifacts"

tap_ok $(for event in 'schema trace_abi=' 'kext state' 'registration component=' \
             'connection action=' 'mount action=' 'runtime request_ms=' \
             'fault type=TIMEOUT' 'rejected opcode=LOOKUP' 'queued unique=' \
             'delivered unique=' 'reply unique=' 'completed unique=' \
             'abort channel=' 'vfs entry operation=ROOT' \
             'vfs result operation=ROOT' 'vnode entry opcode=LOOKUP' \
             'vnode result opcode=LOOKUP' 'pager entry' 'pager result'; do
             grep -q "$event" "$REPORT" || exit 1
         done; echo $?) "every stable trace subhook has symbolic output"

tap_ok $(grep -q 'USFS CONTROL' "$REPORT" &&
         ! grep -q 'UNKNOWN .*SUBHOOK\|UNDEFINED TRACE ID' "$REPORT"; echo $?) \
    "captured records decode without schema fallbacks"

/usr/sbin/usfsctl trace report "$RAW" --output "$EXPLICIT" \
    >"$BASE/report.out" 2>"$BASE/report.err"
tap_ok $(test -s "$EXPLICIT"; echo $?) \
    "saved raw trace can be explicitly reformatted"

touch "$BASE/existing.trc"
/usr/sbin/usfsctl trace start --output "$BASE/existing.trc" >/dev/null 2>&1
tap_ok $(test "$?" -eq 2 -a ! -e /var/adm/ras/usfs.trace.state; echo $?) \
    "an existing requested output is never overwritten"

/usr/bin/trace -a -d -j F5F1 -o "$OTHER" >/dev/null 2>&1
/usr/sbin/usfsctl trace start --output "$BASE/conflict.trc" >/dev/null 2>&1
occupied_rc=$?
owned_state=0
test -e /var/adm/ras/usfs.trace.state && owned_state=1
/usr/bin/trcstop -d >/dev/null 2>&1
tap_ok $(test "$occupied_rc" -eq 2 -a "$owned_state" -eq 0 -a -s "$OTHER"; \
         echo $?) "an occupied channel is refused and left to native administration"

tap_finish

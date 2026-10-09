#!/usr/bin/ksh
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Transaction boundary used by the USFS RPM scriptlets.

set -u

DEVICE=usfs0
UNIQUE_TYPE=cdr/fs/usfs
PDDV_FILE=/usr/lib/objrepos/usfs.pddv.add
CONFIG_RULES_FILE=/usr/lib/objrepos/usfs.config_rules.add
BOOT_RULE=/usr/lib/methods/usfs/boot_rule
ERR_TEMPLATE=/usr/lib/objrepos/usfs.err
ERR_UNDO=/usr/lib/objrepos/usfs.err.undo
TRACE_FORMAT=/usr/lib/ras/usfs.trcfmt
TRACE_UPDATE=/usr/lib/ras/usfs_trace
TRACE_REMOVE=/usr/lib/ras/usfs_trace_remove

trace_templates_owned()
{
    for hook in F5F1 F5F2 F5F3; do
        line=$(/usr/bin/awk -v hook="$hook" '$1 == hook { print; exit }' \
            /etc/trcfmt 2>/dev/null)
        if [ -n "$line" ] && ! echo "$line" | grep -q '"USFS '; then
            echo "usfs: AIX trace hook $hook is owned by another formatter" >&2
            return 1
        fi
    done
    return 0
}

register_trace_templates()
{
    trace_templates_owned || return 1
    /usr/bin/trcrpt -c -t "$TRACE_FORMAT" >/dev/null || return 1
    /usr/bin/trcupdate -o "$TRACE_UPDATE" || return 1
    if ! /usr/bin/trcrpt -c -t /etc/trcfmt >/dev/null; then
        if [ -r "${TRACE_UPDATE}.undo.trc" ]; then
            /usr/bin/trcupdate -o "${TRACE_UPDATE}.undo" >/dev/null 2>&1
        fi
        echo "usfs: registered AIX trace formatter failed validation" >&2
        return 1
    fi
    return 0
}

remove_trace_templates()
{
    trace_templates_owned || return 1
    /usr/bin/trcupdate -o "$TRACE_REMOVE" || return 1
    /usr/bin/trcrpt -c -t /etc/trcfmt >/dev/null
}

device_line()
{
    /usr/sbin/lsdev -l "$DEVICE" 2>/dev/null | grep "^$DEVICE"
}

pre_install()
{
    fault=${1:-none}
    if [ -x /usr/lib/methods/usfs/ucfgusfs ]; then
        # Defined can still own ACTIVE/CLEANUP_REQUIRED kernel state. The
        # direct method reconciles it and verifies DOWN before unloading.
        /usr/lib/methods/usfs/ucfgusfs -l "$DEVICE" || return 1
    else
        records=$(/usr/bin/odmget -q "name=$DEVICE" CuDv) || {
            echo "usfs: cannot verify a fresh installation; repair ODM access and retry before replacing USFS files" >&2
            return 1
        }
        if [ -n "$records" ] || [ -e /usr/lib/drivers/usfs/usfs.kext ] ||
           [ -L /usr/lib/drivers/usfs/usfs.kext ] || [ -e "/dev/$DEVICE" ] ||
           [ -L "/dev/$DEVICE" ]; then
            echo "usfs: teardown method is unavailable; restore the installed ucfgusfs and complete cleanup before retrying" >&2
            return 1
        fi
    fi
    if [ "$fault" = after-unconfigure ]; then
        echo "usfs: injected package interruption after unconfigure" >&2
        return 97
    fi
    return 0
}

post_install()
{
    rule_count=0

    if [ ! -r "$PDDV_FILE" ] || [ ! -r "$CONFIG_RULES_FILE" ] ||
       [ ! -x "$BOOT_RULE" ] || [ ! -r "$ERR_TEMPLATE" ] ||
       [ ! -r "$ERR_UNDO" ] || [ ! -r "$TRACE_FORMAT" ] ||
       [ ! -r "${TRACE_UPDATE}.trc" ] ||
       [ ! -r "${TRACE_REMOVE}.trc" ]; then
        echo "usfs: package ODM or boot inventory is missing" >&2
        return 1
    fi
    /usr/bin/errupdate -f -q "$ERR_TEMPLATE" || return 1
    register_trace_templates || return 1
    if ! /usr/bin/odmget -q "uniquetype=$UNIQUE_TYPE" PdDv 2>/dev/null |
         grep -q "$UNIQUE_TYPE"; then
        /usr/bin/odmadd "$PDDV_FILE" || return 1
    fi
    rule_count=$(/usr/bin/odmget -q "rule=$BOOT_RULE" Config_Rules \
        2>/dev/null | grep -c '^Config_Rules:')
    if [ "$rule_count" -ne 2 ]; then
        if [ "$rule_count" -ne 0 ]; then
            /usr/bin/odmdelete -o Config_Rules -q "rule=$BOOT_RULE" ||
                return 1
        fi
        /usr/bin/odmadd "$CONFIG_RULES_FILE" || return 1
    fi
    if ! device_line >/dev/null; then
        /usr/lib/methods/define -c cdr -s fs -t usfs -n -u || return 1
    fi
    if ! device_line | grep -q Available; then
        /usr/sbin/mkdev -l "$DEVICE" || return 1
    fi
    set -- $(/usr/bin/ls -l "/dev/$DEVICE")
    if [ "$1" != crw-rw---- ] || [ "$3" != root ] ||
       [ "$4" != system ]; then
        echo "usfs: /dev/$DEVICE does not satisfy root:system 0660" >&2
        return 1
    fi
    return 0
}

pre_erase()
{
    pre_install none || return 1
    line=$(device_line)
    if [ -n "$line" ]; then
        /usr/sbin/rmdev -dl "$DEVICE" || return 1
    fi
    if /usr/bin/odmget -q "uniquetype=$UNIQUE_TYPE" PdDv 2>/dev/null |
       grep -q "$UNIQUE_TYPE"; then
        /usr/bin/odmdelete -o PdDv -q "uniquetype=$UNIQUE_TYPE" || return 1
    fi
    if /usr/bin/odmget -q "rule=$BOOT_RULE" Config_Rules 2>/dev/null |
       grep -q "$BOOT_RULE"; then
        /usr/bin/odmdelete -o Config_Rules -q "rule=$BOOT_RULE" || return 1
    fi
    /usr/bin/errupdate -q "$ERR_UNDO" || return 1
    remove_trace_templates || return 1
    return 0
}

case ${1:-} in
    pre-install)
        pre_install "${2:-none}"
        ;;
    post-install)
        post_install
        ;;
    pre-erase)
        pre_erase
        ;;
    register-trace)
        register_trace_templates
        ;;
    *)
        echo "usage: $0 {pre-install [fault]|post-install|pre-erase|register-trace}" >&2
        exit 2
        ;;
esac

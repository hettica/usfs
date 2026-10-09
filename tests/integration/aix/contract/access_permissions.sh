#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify that vnode access checks enforce owner, group, other, directory
#   search, and AIX accessx selector semantics.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/access_permissions.sh

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-access.$$
DAEMON_LOG=$BASE/daemon.log
DAEMON_PID=0
MOUNT=

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

cleanup()
{
    usfs_owned_cleanup
    rm -rf "$BASE"
}

trap cleanup 0 1 2 15
mkdir -p "$BASE"
tap_plan 38

/usr/sbin/usfs_memfs --size=1 >"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
MOUNT=/mnt/$DAEMON_PID/memfs
usfs_track_pid "$DAEMON_PID"
usfs_track_mount "$MOUNT"

if usfs_wait_until "access contract mount" 30 is_mounted; then
    tap_ok 0 "permission-test memfs mounts"
else
    tap_ok 1 "permission-test memfs mounts"
    _n=2
    while [ "$_n" -le 17 ]; do
        tap_ok 1 "permission check unavailable"
        _n=$((_n + 1))
    done
    tap_finish
    exit $?
fi

printf 'protected\n' >"$MOUNT/file"
chmod 600 "$MOUNT/file"
if su nobody -c "cat '$MOUNT/file'" >/dev/null 2>&1; then
    tap_ok 1 "owner-only read is denied to another user"
else
    tap_ok 0 "owner-only read is denied to another user"
fi

chmod 604 "$MOUNT/file"
if su nobody -c "cat '$MOUNT/file'" >/dev/null 2>&1; then
    tap_ok 0 "other read permission allows another user"
else
    tap_ok 1 "other read permission allows another user"
fi

chmod 604 "$MOUNT/file"
if su nobody -c "printf x >>'$MOUNT/file'" >/dev/null 2>&1; then
    tap_ok 1 "missing write permission denies another user"
else
    tap_ok 0 "missing write permission denies another user"
fi

chmod 606 "$MOUNT/file"
if su nobody -c "printf x >>'$MOUNT/file'" >/dev/null 2>&1; then
    tap_ok 0 "other write permission allows another user"
else
    tap_ok 1 "other write permission allows another user"
fi

mkdir "$MOUNT/private"
printf 'inside\n' >"$MOUNT/private/item"
chmod 700 "$MOUNT/private"
chmod 604 "$MOUNT/private/item"
if su nobody -c "cat '$MOUNT/private/item'" >/dev/null 2>&1; then
    tap_ok 1 "directory search is denied without execute permission"
else
    tap_ok 0 "directory search is denied without execute permission"
fi

chmod 701 "$MOUNT/private"
if su nobody -c "cat '$MOUNT/private/item'" >/dev/null 2>&1; then
    tap_ok 0 "directory execute permission allows search"
else
    tap_ok 1 "directory execute permission allows search"
fi

nobody_gid=$(id -g nobody)
chown 0:"$nobody_gid" "$MOUNT/file"
chmod 640 "$MOUNT/file"
if su nobody -c "cat '$MOUNT/file'" >/dev/null 2>&1; then
    tap_ok 0 "credential group membership selects group permissions"
else
    tap_ok 1 "credential group membership selects group permissions"
fi

chmod 000 "$MOUNT/file"
if cat "$MOUNT/file" >/dev/null 2>&1; then
    tap_ok 0 "privileged caller can bypass discretionary read denial"
else
    tap_ok 1 "privileged caller can bypass discretionary read denial"
fi

chmod 400 "$MOUNT/file"
if /usr/sbin/usfs_access_probe "$MOUNT/file" 4 16 >/dev/null 2>&1 &&
   ! /usr/sbin/usfs_access_probe "$MOUNT/file" 4 8 >/dev/null 2>&1 &&
   ! /usr/sbin/usfs_access_probe "$MOUNT/file" 4 32 >/dev/null 2>&1 &&
   chmod 444 "$MOUNT/file" &&
   /usr/sbin/usfs_access_probe "$MOUNT/file" 4 32 >/dev/null 2>&1; then
    tap_ok 0 "ACC_ANY, ACC_OTHERS, and ACC_ALL evaluate permission classes"
else
    tap_ok 1 "ACC_ANY, ACC_OTHERS, and ACC_ALL evaluate permission classes"
fi

chmod 755 "$MOUNT"
if su nobody -c "touch '$MOUNT/new'" >/dev/null 2>&1; then
    tap_ok 1 "directory without other write permission denies create"
else
    tap_ok 0 "directory without other write permission denies create"
fi

chmod 757 "$MOUNT"
if su nobody -c "printf x >'$MOUNT/new'" >"$BASE/create.log" 2>&1; then
    tap_ok 0 "directory write and execute permissions allow create"
else
    sed 's/^/# /' "$BASE/create.log" 2>/dev/null
    ls -l "$MOUNT/new" 2>/dev/null | sed 's/^/# /'
    tap_ok 1 "directory write and execute permissions allow create"
fi

chmod 755 "$MOUNT"
if su nobody -c "rm -f '$MOUNT/new'" >/dev/null 2>&1; then
    tap_ok 1 "directory without other write permission denies remove"
else
    tap_ok 0 "directory without other write permission denies remove"
fi

chmod 757 "$MOUNT"
if su nobody -c "rm -f '$MOUNT/new'" >/dev/null 2>&1; then
    tap_ok 0 "directory write and execute permissions allow remove"
else
    tap_ok 1 "directory write and execute permissions allow remove"
fi

chmod 644 "$MOUNT/file"
if su nobody -c "chmod 777 '$MOUNT/file'" >/dev/null 2>&1; then
    tap_ok 1 "non-owner cannot change object mode"
else
    tap_ok 0 "non-owner cannot change object mode"
fi

printf x >"$MOUNT/unchanged-owner"
chmod 6755 "$MOUNT/unchanged-owner"
if su nobody -c "/usr/sbin/usfs_io_probe unchanged-owner '$MOUNT/unchanged-owner'"; then
    tap_ok 0 "nonowner unchanged-ID chown rejects without changing set-ID mode or ctime"
else
    tap_ok 1 "nonowner unchanged-ID chown rejects without changing set-ID mode or ctime"
fi

chmod 700 "$MOUNT"
if su nobody -c "cat '$MOUNT/file'" >/dev/null 2>&1; then
    tap_ok 1 "restrictive root permissions deny traversal to another user"
else
    tap_ok 0 "restrictive root permissions deny traversal to another user"
fi
if chmod 755 "$MOUNT" && su nobody -c "cat '$MOUNT/file'" >/dev/null 2>&1; then
    tap_ok 0 "authoritative root mode changes restore permitted traversal"
else
    tap_ok 1 "authoritative root mode changes restore permitted traversal"
fi

if /usr/sbin/usfs_io_probe retained-attributes "$MOUNT" >"$BASE/retained.log" 2>&1; then
    tap_ok 0 "unlink and rename replacement preserve the authorized metadata object"
else
    sed 's/^/# /' "$BASE/retained.log"
    tap_ok 1 "unlink and rename replacement preserve the authorized metadata object"
fi

chmod 1777 "$MOUNT"
if su nobody -c "mkdir -m 700 '$MOUNT/caller-private' && printf x >'$MOUNT/caller-private/file' && ln -s file '$MOUNT/caller-private/link'"; then
    tap_ok 0 "caller owns and can use its private directory and symlink"
else
    tap_ok 1 "caller owns and can use its private directory and symlink"
fi
printf x >"$MOUNT/sticky-victim"
if su nobody -c "rm -f '$MOUNT/sticky-victim'" >/dev/null 2>&1; then
    tap_ok 1 "sticky directory rejects another owner's unlink"
else
    tap_ok 0 "sticky directory rejects another owner's unlink"
fi
mkdir "$MOUNT/sticky-directory"
if su nobody -c "rmdir '$MOUNT/sticky-directory'" >/dev/null 2>&1; then
    tap_ok 1 "sticky directory rejects another owner's rmdir"
else
    tap_ok 0 "sticky directory rejects another owner's rmdir"
fi
printf x >"$MOUNT/sticky-target"
su nobody -c "printf y >'$MOUNT/sticky-source'"
if su nobody -c "mv -f '$MOUNT/sticky-source' '$MOUNT/sticky-target'" >/dev/null 2>&1; then
    tap_ok 1 "sticky rename checks the overwritten destination owner"
else
    tap_ok 0 "sticky rename checks the overwritten destination owner"
fi
printf x >"$MOUNT/setid-write"
chmod 6777 "$MOUNT/setid-write"
if su nobody -c "printf y >>'$MOUNT/setid-write'" &&
   [ ! -u "$MOUNT/setid-write" ] && [ ! -g "$MOUNT/setid-write" ]; then
    tap_ok 0 "unprivileged write strips executable set-ID bits"
else
    tap_ok 1 "unprivileged write strips executable set-ID bits"
fi
printf x >"$MOUNT/setid-truncate"
chmod 6777 "$MOUNT/setid-truncate"
if su nobody -c ": >'$MOUNT/setid-truncate'" &&
   [ ! -u "$MOUNT/setid-truncate" ] && [ ! -g "$MOUNT/setid-truncate" ]; then
    tap_ok 0 "unprivileged truncate strips executable set-ID bits"
else
    tap_ok 1 "unprivileged truncate strips executable set-ID bits"
fi
printf x >"$MOUNT/setid-owner"
chmod 6777 "$MOUNT/setid-owner"
if chown nobody "$MOUNT/setid-owner" && [ -u "$MOUNT/setid-owner" ] && [ -g "$MOUNT/setid-owner" ]; then
    tap_ok 0 "privileged ownership change preserves set-ID as on native AIX"
else
    tap_ok 1 "privileged ownership change preserves set-ID as on native AIX"
fi
if su nobody -c "chgrp nobody '$MOUNT/setid-owner'" &&
   [ ! -u "$MOUNT/setid-owner" ] && [ ! -g "$MOUNT/setid-owner" ]; then
    tap_ok 0 "unprivileged group change clears set-ID as on native AIX"
else
    tap_ok 1 "unprivileged group change clears set-ID as on native AIX"
fi

printf x >"$MOUNT/nonexec-setid"
chmod 6666 "$MOUNT/nonexec-setid"
if su nobody -c "printf y >>'$MOUNT/nonexec-setid'" &&
   [ -u "$MOUNT/nonexec-setid" ] && [ -g "$MOUNT/nonexec-setid" ]; then
    tap_ok 0 "writes preserve non-executable set-ID bits as on native AIX"
else
    tap_ok 1 "writes preserve non-executable set-ID bits as on native AIX"
fi
printf x >"$MOUNT/nonmember-setgid"
chown nobody:staff "$MOUNT/nonmember-setgid"
if su nobody -c "chmod 2755 '$MOUNT/nonmember-setgid'" >/dev/null 2>&1 ||
   [ -g "$MOUNT/nonmember-setgid" ]; then
    tap_ok 1 "nonmember cannot enable setgid on a differently grouped file"
else
    tap_ok 0 "nonmember cannot enable setgid on a differently grouped file"
fi
dd if=/dev/zero of="$MOUNT/mapped-setid" bs=4096 count=1 2>/dev/null
chmod 6777 "$MOUNT/mapped-setid"
if su nobody -c "/usr/sbin/usfs_io_probe mmap-write-shared '$MOUNT/mapped-setid' 4096 0" >"$BASE/mapped-setid.log" 2>&1 &&
   [ ! -u "$MOUNT/mapped-setid" ] && [ ! -g "$MOUNT/mapped-setid" ]; then
    tap_ok 0 "unprivileged shared mapping removes executable set-ID privileges"
else
    tap_ok 1 "unprivileged shared mapping removes executable set-ID privileges"
fi

for kind in shared fork upgrade upgrade-privileged private readonly privileged race; do
    if /usr/sbin/usfs_io_probe mapped-setid-policy "$MOUNT" "$kind"; then
        tap_ok 0 "mapped set-ID admission and reclamation policy: $kind"
    else
        tap_ok 1 "mapped set-ID admission and reclamation policy: $kind"
    fi
done

tap_finish

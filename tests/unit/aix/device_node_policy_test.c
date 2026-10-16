/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */
#include "tap.h"
#include "usfs_common.h"

enum
{
    TEST_OWNER_ID = 17,
    TEST_GROUP_ID = 23
};

static int owner_error, group_error, owner_missing, group_missing;
static int owner_retry, group_retry, allocation_failure, ownership_failure, mode_failure;
static unsigned owner_calls, group_calls, ownership_calls, mode_calls, removal_calls;

static int lookup_test_owner (const char * name, struct passwd * account, char * buffer, int capacity, struct passwd ** result)
{
    (void)buffer;
    if (strcmp (name, USFS_DEVICE_NODE_OWNER) != 0 || capacity <= 0)
        abort ();

    ++owner_calls;
    *result = NULL;

    if (owner_retry && owner_calls == 1)
        return ERANGE;

    if (owner_error != 0)
        return owner_error;

    if (!owner_missing)
    {
        account->pw_uid = TEST_OWNER_ID;
        *result = account;
    }

    return 0;
}

static int lookup_test_group (const char * name, struct group * account, char * buffer, int capacity, struct group ** result)
{
    (void)buffer;
    if (strcmp (name, USFS_DEVICE_NODE_GROUP) != 0 || capacity <= 0)
        abort ();

    ++group_calls;
    *result = NULL;

    if (group_retry && group_calls == 1)
        return ERANGE;

    if (group_error != 0)
        return group_error;

    if (!group_missing)
    {
        account->gr_gid = TEST_GROUP_ID;
        *result = account;
    }

    return 0;
}

static void * resize_test_buffer (void * buffer, size_t capacity)
{
    if (allocation_failure)
        return NULL;

    return realloc (buffer, capacity);
}

static int set_test_ownership (const char * path, uid_t owner, gid_t group)
{
    if (strcmp (path, "/test-device") != 0 || owner != TEST_OWNER_ID || group != TEST_GROUP_ID)
        abort ();

    ++ownership_calls;
    return ownership_failure ? -1 : 0;
}

static int set_test_mode (const char * path, mode_t mode)
{
    if (strcmp (path, "/test-device") != 0 || mode != USFS_DEVICE_NODE_PERMS)
        abort ();

    ++mode_calls;
    return mode_failure ? -1 : 0;
}

static int remove_test_node (const char * path)
{
    if (strcmp (path, "/test-device") != 0)
        abort ();

    ++removal_calls;
    return 0;
}

#define getpwnam_r lookup_test_owner
#define getgrnam_r lookup_test_group
#define realloc    resize_test_buffer
#define chown      set_test_ownership
#define chmod      set_test_mode
#define unlink     remove_test_node
#include "../../../src/common/usfs_common.c"
#undef unlink
#undef chmod
#undef chown
#undef realloc
#undef getgrnam_r
#undef getpwnam_r

static void reset_policy_case (void)
{
    owner_error = group_error = owner_missing = group_missing = 0;
    owner_retry = group_retry = allocation_failure = ownership_failure = mode_failure = 0;
    owner_calls = group_calls = ownership_calls = mode_calls = removal_calls = 0;
}

static void test_device_node_policy (struct tap_state * tap)
{
    reset_policy_case ();
    tap_ok (
        tap,
        apply_device_node_policy ("/test-device") == USFS_SUCCESS && owner_calls == 1 && group_calls == 1 && ownership_calls == 1 &&
            mode_calls == 1 && removal_calls == 0,
        "complete account policy sets the intended ownership and permissions"
    );

    reset_policy_case ();
    owner_retry = group_retry = 1;
    tap_ok (
        tap,
        apply_device_node_policy ("/test-device") == USFS_SUCCESS && owner_calls == 2 && group_calls == 2 && ownership_calls == 1 &&
            mode_calls == 1 && removal_calls == 0,
        "reentrant lookups retry both account databases after ERANGE"
    );

    reset_policy_case ();
    owner_missing = 1;
    tap_ok (
        tap,
        apply_device_node_policy ("/test-device") == USFS_FAILURE && group_calls == 0 && ownership_calls == 0 && mode_calls == 0 &&
            removal_calls == 1,
        "missing owner fails closed and removes the node"
    );

    reset_policy_case ();
    group_missing = 1;
    tap_ok (
        tap,
        apply_device_node_policy ("/test-device") == USFS_FAILURE && ownership_calls == 0 && mode_calls == 0 && removal_calls == 1,
        "missing group fails closed and removes the node"
    );

    reset_policy_case ();
    owner_error = EIO;
    tap_ok (
        tap,
        apply_device_node_policy ("/test-device") == USFS_FAILURE && group_calls == 0 && ownership_calls == 0 && removal_calls == 1,
        "owner lookup error prevents policy mutation"
    );

    reset_policy_case ();
    group_error = EIO;
    tap_ok (
        tap,
        apply_device_node_policy ("/test-device") == USFS_FAILURE && ownership_calls == 0 && removal_calls == 1,
        "group lookup error prevents policy mutation"
    );

    reset_policy_case ();
    allocation_failure = 1;
    tap_ok (
        tap,
        apply_device_node_policy ("/test-device") == USFS_FAILURE && owner_calls == 0 && group_calls == 0 && ownership_calls == 0 &&
            removal_calls == 1,
        "lookup allocation failure removes the unconfigured node"
    );

    reset_policy_case ();
    ownership_failure = 1;
    tap_ok (
        tap,
        apply_device_node_policy ("/test-device") == USFS_FAILURE && mode_calls == 0 && removal_calls == 1,
        "ownership failure prevents chmod and removes the node"
    );

    reset_policy_case ();
    mode_failure = 1;
    tap_ok (
        tap,
        apply_device_node_policy ("/test-device") == USFS_FAILURE && ownership_calls == 1 && removal_calls == 1,
        "permission failure removes the partially configured node"
    );
}

static void test_vfs_copy (struct tap_state * tap)
{
    FILE * source = tmpfile ();
    FILE * target = tmpfile ();
    if (source == NULL || target == NULL)
        abort ();

    int last_character = '\n';

    tap_ok (
        tap,
        copy_other_vfs_entries (NULL, target, &last_character) == USFS_SUCCESS && ftell (target) == 0,
        "missing VFS source leaves the replacement empty"
    );

    fputs ("other 1 none\nusfs 7 none\nlast", source);
    rewind (source);
    int rc = copy_other_vfs_entries (source, target, &last_character);
    char contents[32] = { 0 };

    rewind (target);
    const size_t copied = fread (contents, 1, sizeof (contents) - 1, target);
    tap_ok (
        tap,
        rc == USFS_SUCCESS && copied == strlen ("other 1 none\nlast") && strcmp (contents, "other 1 none\nlast") == 0 && last_character == 't',
        "VFS copy filters USFS and preserves an unterminated final line"
    );

    fclose (source);
    fclose (target);
    source = tmpfile ();
    target = tmpfile ();
    if (source == NULL || target == NULL)
        abort ();

    const char malformed[] = "other\0hidden\n";

    fwrite (malformed, 1, sizeof (malformed) - 1, source);
    rewind (source);
    rc = copy_other_vfs_entries (source, target, &last_character);
    tap_ok (tap, rc == USFS_FAILURE && ftell (target) == 0, "VFS copy rejects a malformed source before writing it");
    fclose (source);
    fclose (target);
}

int main (void)
{
    struct tap_state tap;

    tap_plan (&tap, 12);
    test_device_node_policy (&tap);
    test_vfs_copy (&tap);
    return tap_finish (&tap);
}

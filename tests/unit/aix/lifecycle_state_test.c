/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"

#include <string.h>

static unsigned lock_depth;
static unsigned lock_calls;
static unsigned unlock_calls;
static unsigned mount_release_observation_calls;
static unsigned close_wakes;
static int close_order_valid;
void wake_closed_connection(struct usfs_connection *connection)
{
    ++close_wakes;
    close_order_valid = lock_depth == 0 && connection->mounts_counter == 0 && g_mount_count == 0;
}

void on_mount_released(struct usfs_connection *connection)
{
    (void)connection;
    mount_release_observation_calls += 1;
}

void simple_lock(Simple_lock *lock)
{
    (void)lock;
    lock_depth += 1;
    lock_calls += 1;
}

void simple_unlock(Simple_lock *lock)
{
    (void)lock;
    lock_depth -= 1;
    unlock_calls += 1;
}

static void reset_state(void)
{
    memset(g_connections, 0, sizeof(g_connections));
    g_mount_count = 0;
    usfs_lifecycle_state_replace(USFS_KEXT_ACTIVE);
    g_gate_is_open = 1;
    g_kext_is_running_control_operation = 0;
    lock_depth = 0;
    lock_calls = 0;
    unlock_calls = 0;
    mount_release_observation_calls = 0;
}

int main(void)
{
    struct tap_state tap;
    struct usfs_connection first;
    struct usfs_connection second;
    struct usfs_connection *reserved;

    tap_plan(&tap, 38);
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));

    reset_state();
    tap_ok(&tap, has_active_connections() == 0,
           "empty connection table is reported empty");
    g_connections[3] = &first;
    tap_ok(&tap, has_active_connections() == 1,
           "occupied connection table is reported nonempty");

    g_gate_is_open = 0;
    tap_ok(&tap, usfs_lifecycle_control_enter(
                       3, USFS_CONTROL_EXACTLY_ONE_CONNECTION) == EBUSY,
           "control operation requires an accepting lifecycle");
    g_gate_is_open = 1;
    g_kext_is_running_control_operation = 1;
    tap_ok(&tap, usfs_lifecycle_control_enter(
                       3, USFS_CONTROL_EXACTLY_ONE_CONNECTION) == EBUSY,
           "control operations are mutually exclusive");
    g_kext_is_running_control_operation = 0;
    usfs_lifecycle_state_replace(USFS_KEXT_DOWN);
    tap_ok(&tap, usfs_lifecycle_control_enter(
                       3, USFS_CONTROL_EXACTLY_ONE_CONNECTION) == EBUSY,
           "control operation requires an active extension");
    usfs_lifecycle_state_replace(USFS_KEXT_ACTIVE);
    g_mount_count = 1;
    tap_ok(&tap, usfs_lifecycle_control_enter(
                       3, USFS_CONTROL_EXACTLY_ONE_CONNECTION) == EBUSY,
           "control operation rejects an active mount");
    g_mount_count = 0;

    tap_ok(&tap, usfs_lifecycle_control_enter(
                       -1, USFS_CONTROL_ONLY_COLLECTOR_CONNECTION) == ENXIO,
           "collector-only policy distinguishes an invalid collector");
    tap_ok(&tap, usfs_lifecycle_control_enter(
                       4, USFS_CONTROL_EXACTLY_ONE_CONNECTION) == EBUSY,
           "exactly-one policy rejects an absent collector");
    tap_ok(&tap, usfs_lifecycle_control_enter(
                       3, USFS_CONTROL_EXACTLY_ONE_CONNECTION) == 0 &&
                   g_kext_is_running_control_operation == 1,
           "exactly-one policy admits its sole connection");
    usfs_lifecycle_control_leave();
    tap_ok(&tap, g_kext_is_running_control_operation == 0,
           "leaving a control operation reopens admission");

    g_connections[5] = &second;
    tap_ok(&tap, usfs_lifecycle_control_enter(
                       3, USFS_CONTROL_EXACTLY_ONE_CONNECTION) == EBUSY,
           "exactly-one policy rejects multiple connections");
    tap_ok(&tap, usfs_lifecycle_control_enter(
                       3, USFS_CONTROL_ONLY_COLLECTOR_CONNECTION) == EBUSY,
           "collector-only policy rejects a peer connection");
    g_connections[5] = NULL;
    tap_ok(&tap, usfs_lifecycle_control_enter(
                       3, USFS_CONTROL_ONLY_COLLECTOR_CONNECTION) == 0,
           "collector-only policy admits an isolated collector");
    usfs_lifecycle_control_leave();

    first.cookie = 0x1234;
    first.state = USFS_CONN_ACTIVE;
    first.refs_counter = 2;
    reserved = &second;
    g_gate_is_open = 0;
    tap_ok(&tap, usfs_lifecycle_reserve_mount(3, first.cookie, &reserved) ==
                       EBUSY && reserved == NULL,
           "mount reservation requires an accepting lifecycle");
    g_gate_is_open = 1;
    g_kext_is_running_control_operation = 1;
    tap_ok(&tap, usfs_lifecycle_reserve_mount(3, first.cookie, &reserved) ==
                       EBUSY && reserved == NULL,
           "mount reservation is excluded by a control operation");
    g_kext_is_running_control_operation = 0;
    usfs_lifecycle_state_replace(USFS_KEXT_DOWN);
    tap_ok(&tap, usfs_lifecycle_reserve_mount(3, first.cookie, &reserved) ==
                       EBUSY && reserved == NULL,
           "mount reservation requires an active extension");
    usfs_lifecycle_state_replace(USFS_KEXT_ACTIVE);
    tap_ok(&tap, usfs_lifecycle_reserve_mount(USFS_MAX_CONNECTIONS, first.cookie,
                                              &reserved) == EINVAL &&
                   reserved == NULL,
           "mount reservation rejects an out-of-range channel");
    tap_ok(&tap, usfs_lifecycle_reserve_mount(4, first.cookie, &reserved) ==
                       EINVAL && reserved == NULL,
           "mount reservation rejects an unused channel");
    tap_ok(&tap, usfs_lifecycle_reserve_mount(3, 0xbeef, &reserved) == EINVAL &&
                   reserved == NULL,
           "mount reservation rejects the wrong cookie");
    first.state = USFS_CONN_DEAD;
    tap_ok(&tap, usfs_lifecycle_reserve_mount(3, first.cookie, &reserved) ==
                       EINVAL && reserved == NULL,
           "mount reservation rejects an inactive connection");
    first.state = USFS_CONN_ACTIVE;
    tap_ok(&tap, usfs_lifecycle_reserve_mount(3, first.cookie, &reserved) == 0 &&
                   reserved == &first,
           "valid mount reservation returns its connection");
    tap_ok(&tap, first.refs_counter == 3 && first.mounts_counter == 1 && g_mount_count == 1,
           "valid mount reservation owns a reference and mount slot");
    tap_ok(&tap, usfs_lifecycle_reserve_mount(3, first.cookie, &reserved) == EBUSY &&
                   reserved == NULL && first.refs_counter == 3 &&
                   first.mounts_counter == 1 && g_mount_count == 1,
           "second binding is rejected without changing ownership");
    usfs_lifecycle_release_mount(&first);
    tap_ok(&tap, first.mounts_counter == 0 && g_mount_count == 0,
           "mount release relinquishes the mount slot");
    usfs_lifecycle_release_mount(&first);
    tap_ok(&tap, first.mounts_counter == 0 && g_mount_count == 0 &&
                     mount_release_observation_calls == 2,
           "extra mount release cannot underflow the count");

    first.state = USFS_CONN_ACTIVE; first.mounts_counter = g_mount_count = 1;
    usfs_lifecycle_finish_mount(&first);
    tap_ok(&tap, first.state == USFS_CONN_CLOSED && close_wakes == 1 && close_order_valid &&
           usfs_lifecycle_reserve_mount(3, first.cookie, &reserved) == EINVAL,
           "ordinary completion closes after accounting, wakes outside locks and forbids channel remount");
    first.state = USFS_CONN_UNHEALTHY; first.mounts_counter = g_mount_count = 1;
    usfs_lifecycle_finish_mount(&first);
    tap_ok(&tap, first.state == USFS_CONN_UNHEALTHY && close_order_valid,
           "ordinary cleanup preserves an earlier timeout state");
    first.state = USFS_CONN_DEAD; first.mounts_counter = g_mount_count = 1;
    usfs_lifecycle_finish_mount(&first);
    tap_ok(&tap, first.state == USFS_CONN_DEAD && close_order_valid,
           "ordinary cleanup preserves an earlier transport failure");
    tap_ok(&tap, lock_calls == unlock_calls,
           "every lifecycle lock acquisition is released");
    tap_ok(&tap, lock_depth == 0,
           "lifecycle operations leave no lock held");
    tap_ok(&tap, lock_calls > 0,
           "lifecycle tests execute through real lock scopes");

    tap_ok(&tap, usfs_lifecycle_mpx_enter(0) == EBUSY &&
                    !usfs_lifecycle_mpx_has_users(),
           "sealed callback guard refuses allocations without a lease");
    tap_ok(&tap, usfs_lifecycle_mpx_activate() == 0 &&
                    usfs_lifecycle_mpx_enter(0) == 0 &&
                    usfs_lifecycle_mpx_has_users(),
           "active callback guard grants an allocation lease");
    tap_ok(&tap, usfs_lifecycle_mpx_begin_close() == 0 &&
                    usfs_lifecycle_mpx_enter(0) == EBUSY,
           "closing callback guard refuses new allocations");
    tap_ok(&tap, usfs_lifecycle_mpx_enter(1) == 0 &&
                    !usfs_lifecycle_mpx_seal(),
           "closing callback guard admits final deallocation and prevents sealing");
    usfs_lifecycle_mpx_leave();
    usfs_lifecycle_mpx_leave();
    tap_ok(&tap, !usfs_lifecycle_mpx_has_users() &&
                    usfs_lifecycle_mpx_seal(),
           "callback guard seals only after every lease returns");
    tap_ok(&tap, usfs_lifecycle_mpx_enter(0) == EBUSY &&
                    usfs_lifecycle_mpx_enter(1) == EBUSY,
           "sealed callback guard refuses both callback classes");
    usfs_lifecycle_mpx_reopen();
    tap_ok(&tap, usfs_lifecycle_mpx_enter(0) == 0,
           "rollback reopens callback admission");
    usfs_lifecycle_mpx_leave();

    return tap_finish(&tap);
}

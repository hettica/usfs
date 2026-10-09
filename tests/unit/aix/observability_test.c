/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include <stdlib.h>
#include "definitions.h"
#include <sys/time.h>
#include <string.h>

struct usfs_connection *g_connections[USFS_MAX_CONNECTIONS];
Simple_lock g_connections_table_lock, g_lifecycle_lock;
struct usfs_runtime_limits g_usfs_runtime_limits;
int g_mount_count;
static unsigned lock_depth;
static long snapshot_ticks;
static time_t snapshot_time;

void simple_lock(Simple_lock *lock) { (void)lock; ++lock_depth; }
void simple_unlock(Simple_lock *lock) { (void)lock; if (!lock_depth) abort(); --lock_depth; }
int usfs_lifecycle_state_read(void) { return USFS_KEXT_ACTIVE; }
void usfs_ras_report(uint32_t fault, int error, int channel, uint16_t opcode,
                     uint64_t unique, uint32_t mounts)
{ (void)fault; (void)error; (void)channel; (void)opcode; (void)unique; (void)mounts; }

/* Only kernel infrastructure is substituted; exercise the complete production
 * snapshot, including its initialization, filtering, aggregation and locks. */
#undef lock_alloc
#undef simple_lock_init
#undef lock_free
#define lock_alloc(lock, flags, cls, occurrence) \
    ((void)(lock), (void)(flags), (void)(cls), (void)(occurrence))
#define simple_lock_init(...) ((void)0)
#define lock_free(...) ((void)0)
#define lbolt snapshot_ticks
#define time snapshot_time
#include "../../../src/kernel/device/observability.c"

int main(void)
{
    struct tap_state tap;
    struct usfs_connection first = {0}, second = {0}, idle = {0};
    struct usfs_request pending[3] = {{0}}, delivered[4] = {{0}};
    struct kext_state state;
    tap_plan(&tap, 7);
    first.ready = second.ready = idle.ready = 1;
    first.state = second.state = idle.state = USFS_CONN_ACTIVE;
    pending[0].next = &pending[1];
    delivered[0].next = &delivered[1]; delivered[1].next = &delivered[2];
    first.pending_requests_head = &pending[0]; first.delivered_requests_head = &delivered[0];
    first.outstanding_requests = 5;
    second.pending_requests_head = &pending[2]; second.delivered_requests_head = &delivered[3];
    second.outstanding_requests = 2;
    g_connections[1] = &first; g_connections[2] = &second; g_connections[3] = &idle;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        if (scenario == 1) { g_connections[1] = &second; g_connections[2] = &first; }
        if (scenario == 3) second.ready = 0;
        if (scenario == 5) memset(g_connections, 0, sizeof(g_connections));
        unsigned p = scenario == 5 ? 0 : scenario >= 2 && scenario < 4 ? 2 : 3;
        unsigned d = scenario == 5 ? 0 : scenario >= 2 && scenario < 4 ? 3 : 4;
        if (scenario == 4) second.ready = 1;
        memset(&state, 0xff, sizeof(state));
        int rc = take_kext_state_snapshot(&state, scenario == 2 ? 1 : 0);
        tap_ok(&tap, rc == 0 && state.pending_requests == p && state.delivered_requests == d &&
               state.outstanding_requests == p + d && lock_depth == 0 &&
               state.magic == USFS_STATUS_MAGIC && state.abi_version == USFS_STATUS_ABI_VERSION,
               "snapshot totals are order independent and exclude only the collector and non-ready connections");
    }
    memset(g_connections, 0, sizeof(g_connections));
    memset(&first, 0, sizeof(first));
    first.ready = 1; first.state = USFS_CONN_CLOSED;
    g_connections[1] = &first;
    on_connection_failed(&first, USFS_CONN_ACTIVE, USFS_CONN_CLOSED);
    tap_ok(&tap, take_kext_state_snapshot(&state, 0) == 0 &&
           state.active_daemons == 0 && state.unhealthy_daemons == 0 &&
           state.dead_mounted_daemons == 0 && state.health == USFS_HEALTH_HEALTHY &&
           first.degraded_mount_counted == 0,
           "orderly-closed channels are inactive without degrading filesystem health");
    return tap_finish(&tap);
}

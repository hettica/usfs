/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"

#include <string.h>

struct usfs_connection *g_connections[USFS_MAX_CONNECTIONS];
Simple_lock g_connections_table_lock;
struct usfs_runtime_limits g_usfs_runtime_limits;

static unsigned lock_depth;
static unsigned lock_calls;
static unsigned unlock_calls;

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

static struct kext_runtime_config valid_config(void)
{
    struct kext_runtime_config config;

    memset(&config, 0, sizeof(config));
    config.magic = USFS_RUNTIME_CONFIG_MAGIC;
    config.abi_version = USFS_RUNTIME_CONFIG_ABI_VERSION;
    config.size = sizeof(config);
    config.set_mask = USFS_RUNTIME_SET_ALL;
    config.request_timeout_ms = USFS_DEFAULT_REQUEST_TIMEOUT_MS;
    config.pager_timeout_ms = USFS_DEFAULT_PAGER_TIMEOUT_MS;
    config.max_outstanding_requests = USFS_DEFAULT_MAX_OUTSTANDING;
    return config;
}

static void expect_rejected(struct tap_state *tap,
                            struct kext_runtime_config config,
                            const char *description)
{
    tap_ok(tap, set_kext_runtime_config(&config) == EINVAL, description);
}

static void reset_state(void)
{
    memset(g_connections, 0, sizeof(g_connections));
    g_usfs_runtime_limits.request_timeout_ms = USFS_DEFAULT_REQUEST_TIMEOUT_MS;
    g_usfs_runtime_limits.pager_timeout_ms = USFS_DEFAULT_PAGER_TIMEOUT_MS;
    g_usfs_runtime_limits.max_outstanding_requests = USFS_DEFAULT_MAX_OUTSTANDING;
    lock_depth = 0;
    lock_calls = 0;
    unlock_calls = 0;
}

int main(void)
{
    struct tap_state tap;
    struct kext_runtime_config config;
    struct kext_runtime_config snapshot;
    struct usfs_connection active;
    struct usfs_connection unhealthy;
    struct usfs_connection dead;

    tap_plan(&tap, 30);
    reset_state();
    memset(&active, 0, sizeof(active));
    memset(&unhealthy, 0, sizeof(unhealthy));
    memset(&dead, 0, sizeof(dead));

    tap_ok(&tap, take_kext_runtime_config_snapshot(NULL) == EINVAL,
           "snapshot rejects a null destination");
    tap_ok(&tap, set_kext_runtime_config(NULL) == EINVAL,
           "update rejects a null configuration");

    config = valid_config();
    config.magic = 0;
    expect_rejected(&tap, config, "update rejects the wrong magic");
    config = valid_config();
    config.abi_version += 1;
    expect_rejected(&tap, config, "update rejects the wrong ABI version");
    config = valid_config();
    config.size -= 1;
    expect_rejected(&tap, config, "update rejects the wrong structure size");
    config = valid_config();
    config.set_mask = 0;
    expect_rejected(&tap, config, "update requires at least one selected field");
    config = valid_config();
    config.set_mask |= 0x80000000u;
    expect_rejected(&tap, config, "update rejects unknown selection bits");

    config = valid_config();
    config.request_timeout_ms = USFS_MIN_TIMEOUT_MS - 1;
    expect_rejected(&tap, config, "request timeout rejects a value below its minimum");
    config = valid_config();
    config.request_timeout_ms = USFS_MAX_TIMEOUT_MS + 1;
    expect_rejected(&tap, config, "request timeout rejects a value above its maximum");
    config = valid_config();
    config.pager_timeout_ms = USFS_MIN_TIMEOUT_MS - 1;
    expect_rejected(&tap, config, "pager timeout rejects a value below its minimum");
    config = valid_config();
    config.pager_timeout_ms = USFS_MAX_TIMEOUT_MS + 1;
    expect_rejected(&tap, config, "pager timeout rejects a value above its maximum");
    config = valid_config();
    config.max_outstanding_requests = USFS_MIN_OUTSTANDING - 1;
    expect_rejected(&tap, config, "outstanding limit rejects a value below its minimum");
    config = valid_config();
    config.max_outstanding_requests = USFS_MAX_OUTSTANDING + 1;
    expect_rejected(&tap, config, "outstanding limit rejects a value above its maximum");
    config = valid_config();
    config.reserved[2] = 1;
    expect_rejected(&tap, config, "update requires reserved fields to be zero");
    tap_ok(&tap,
           g_usfs_runtime_limits.request_timeout_ms == USFS_DEFAULT_REQUEST_TIMEOUT_MS &&
               g_usfs_runtime_limits.pager_timeout_ms == USFS_DEFAULT_PAGER_TIMEOUT_MS &&
               g_usfs_runtime_limits.max_outstanding_requests == USFS_DEFAULT_MAX_OUTSTANDING,
           "rejected updates preserve effective limits");

    active.state = USFS_CONN_ACTIVE;
    active.outstanding_requests = 3;
    unhealthy.state = USFS_CONN_UNHEALTHY;
    unhealthy.outstanding_requests = 5;
    dead.state = USFS_CONN_DEAD;
    dead.outstanding_requests = 7;
    g_connections[1] = &active;
    g_connections[7] = &unhealthy;
    g_connections[USFS_MAX_CONNECTIONS - 1] = &dead;

    config = valid_config();
    config.request_timeout_ms = USFS_MIN_TIMEOUT_MS;
    config.pager_timeout_ms = USFS_MAX_TIMEOUT_MS;
    config.max_outstanding_requests = USFS_MAX_OUTSTANDING;
    tap_ok(&tap, set_kext_runtime_config(&config) == 0,
           "update accepts all inclusive boundary values");
    tap_ok(&tap,
           g_usfs_runtime_limits.request_timeout_ms == USFS_MIN_TIMEOUT_MS &&
               g_usfs_runtime_limits.pager_timeout_ms == USFS_MAX_TIMEOUT_MS &&
               g_usfs_runtime_limits.max_outstanding_requests == USFS_MAX_OUTSTANDING,
           "all-field update changes the global defaults");
    tap_ok(&tap,
           active.request_timeout_ms == USFS_MIN_TIMEOUT_MS &&
               unhealthy.pager_timeout_ms == USFS_MAX_TIMEOUT_MS &&
               dead.max_outstanding_requests == USFS_MAX_OUTSTANDING,
           "all-field update propagates to every existing connection");

    memset(&snapshot, 0xa5, sizeof(snapshot));
    tap_ok(&tap, take_kext_runtime_config_snapshot(&snapshot) == 0,
           "snapshot succeeds with a valid destination");
    tap_ok(&tap,
           snapshot.magic == USFS_RUNTIME_CONFIG_MAGIC &&
               snapshot.abi_version == USFS_RUNTIME_CONFIG_ABI_VERSION &&
               snapshot.size == sizeof(snapshot),
           "snapshot returns the public ABI header");
    tap_ok(&tap,
           snapshot.request_timeout_ms == USFS_MIN_TIMEOUT_MS &&
               snapshot.pager_timeout_ms == USFS_MAX_TIMEOUT_MS &&
               snapshot.max_outstanding_requests == USFS_MAX_OUTSTANDING,
           "snapshot returns the effective limits");
    tap_ok(&tap,
           snapshot.active_connections == 1 &&
               snapshot.unhealthy_connections == 1,
           "snapshot classifies active and unhealthy connections");
    tap_ok(&tap, snapshot.outstanding_requests == 15,
           "snapshot totals outstanding requests across existing connections");
    tap_ok(&tap,
           snapshot.set_mask == 0 && snapshot.reserved[0] == 0 &&
               snapshot.reserved[1] == 0 && snapshot.reserved[2] == 0,
           "snapshot clears input-only and reserved fields");

    config = valid_config();
    config.set_mask = USFS_RUNTIME_SET_PAGER_TIMEOUT;
    config.request_timeout_ms = 0;
    config.pager_timeout_ms = 4321;
    config.max_outstanding_requests = 0;
    tap_ok(&tap, set_kext_runtime_config(&config) == 0,
           "selective update ignores unselected field values");
    tap_ok(&tap,
           g_usfs_runtime_limits.request_timeout_ms == USFS_MIN_TIMEOUT_MS &&
               g_usfs_runtime_limits.pager_timeout_ms == 4321 &&
               g_usfs_runtime_limits.max_outstanding_requests == USFS_MAX_OUTSTANDING,
           "selective update changes only the chosen global limit");
    tap_ok(&tap,
           active.request_timeout_ms == USFS_MIN_TIMEOUT_MS &&
               active.pager_timeout_ms == 4321 &&
               active.max_outstanding_requests == USFS_MAX_OUTSTANDING,
           "selective update changes only the chosen connection limit");

    tap_ok(&tap, lock_calls == unlock_calls,
           "runtime configuration releases every acquired lock");
    tap_ok(&tap, lock_depth == 0,
           "runtime configuration leaves no lock held");
    tap_ok(&tap, lock_calls > 0,
           "runtime configuration tests execute real lock scopes");
    return tap_finish(&tap);
}

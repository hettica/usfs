// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_STATUS_H
#define USFS_STATUS_H

#ifdef _KERNEL
    #include <sys/inttypes.h>
#else
    #include <stdint.h>
#endif

#define USFS_STATUS_MAGIC           0x55534653u
#define USFS_STATUS_ABI_VERSION     1u
#define USFS_STATUS_LATENCY_BUCKETS 20u

#define USFS_HEALTH_HEALTHY  1u
#define USFS_HEALTH_DEGRADED 2u

#define USFS_HEALTH_REASON_UNHEALTHY_DAEMON 0x00000001u
#define USFS_HEALTH_REASON_DEAD_MOUNT       0x00000002u
#define USFS_HEALTH_REASON_STALE_RESOURCES  0x00000004u
#define USFS_HEALTH_REASON_CLEANUP_REQUIRED 0x00000008u

#define USFS_FAULT_NONE             0u
#define USFS_FAULT_TIMEOUT          1u
#define USFS_FAULT_PROTOCOL         2u
#define USFS_FAULT_DAEMON_LOST      3u
#define USFS_FAULT_FORCED_RECOVERY  4u
#define USFS_FAULT_CLEANUP_REQUIRED 5u

struct usfs_latency_stats
{
    uint64_t samples;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t buckets[USFS_STATUS_LATENCY_BUCKETS];
};

struct kext_state
{
    uint32_t magic;
    uint16_t abi_version;
    uint16_t size;

    uint32_t kext_state;
    uint32_t health;
    uint32_t health_reasons;
    uint32_t reserved0;

    uint32_t request_timeout_ms;
    uint32_t pager_timeout_ms;
    uint32_t max_outstanding_requests;
    uint32_t active_mounts;
    uint32_t stale_mounts;
    uint32_t active_daemons;
    uint32_t unhealthy_daemons;
    uint32_t dead_mounted_daemons;
    uint32_t pending_requests;
    uint32_t delivered_requests;
    uint32_t outstanding_requests;
    uint32_t peak_outstanding_requests;

    uint64_t accepted_requests;
    uint64_t successful_requests;
    uint64_t daemon_error_requests;
    uint64_t transport_failure_requests;
    uint64_t timed_out_requests;
    uint64_t queue_rejections;
    uint64_t interrupted_requests;
    uint64_t protocol_errors;
    uint64_t daemon_disconnects;
    uint64_t forced_recoveries;

    struct usfs_latency_stats normal_latency;
    struct usfs_latency_stats pager_latency;

    uint32_t last_fault;
    int32_t last_fault_errno;
    int32_t last_fault_channel;
    uint16_t last_fault_opcode;
    uint16_t reserved1;
    uint64_t last_fault_unique;
    uint64_t last_fault_time_sec;
    uint64_t reserved[4];
};

extern const uint32_t
    usfs_status_latency_bounds_ms[USFS_STATUS_LATENCY_BUCKETS];

int usfs_status_validate (const struct kext_state * status);
uint32_t usfs_status_percentile_ms (const struct usfs_latency_stats * stats, uint32_t percentile);

#endif

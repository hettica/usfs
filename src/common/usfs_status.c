// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "usfs_status.h"

#include <stddef.h>

const uint32_t usfs_status_latency_bounds_ms[USFS_STATUS_LATENCY_BUCKETS] = {
    1u,
    2u,
    4u,
    8u,
    16u,
    32u,
    64u,
    128u,
    256u,
    512u,
    1000u,
    2000u,
    5000u,
    10000u,
    30000u,
    60000u,
    120000u,
    300000u,
    600000u,
    UINT32_MAX
};

int usfs_status_validate (const struct kext_state * status)
{
    if (status == NULL)
        return -1;

    if (status->magic != USFS_STATUS_MAGIC)
        return -1;

    if (status->abi_version != USFS_STATUS_ABI_VERSION)
        return -1;

    if (status->size != sizeof (*status))
        return -1;

    if (status->reserved0 != 0)
        return -1;

    if (status->reserved1 != 0)
        return -1;

    if (status->health != USFS_HEALTH_HEALTHY && status->health != USFS_HEALTH_DEGRADED)
        return -1;

    for (size_t reserved_index = 0;
         reserved_index < sizeof (status->reserved) / sizeof (status->reserved[0]);
         ++reserved_index)
    {
        if (status->reserved[reserved_index] != 0)
            return -1;
    }

    return 0;
}

static uint64_t calculate_percentile_sample (const uint64_t sample_count, const uint32_t percentile)
{
    const uint64_t whole_hundreds = sample_count / 100u;
    const uint64_t remaining_samples = sample_count % 100u;

    return whole_hundreds * percentile + (remaining_samples * percentile + 99u) / 100u;
}

uint32_t usfs_status_percentile_ms (const struct usfs_latency_stats * latency_stats, const uint32_t percentile)
{
    if (latency_stats == NULL)
        return 0;

    if (latency_stats->samples == 0)
        return 0;

    if (percentile == 0)
        return 0;

    if (percentile > 100)
        return 0;

    const uint64_t target_sample = calculate_percentile_sample (latency_stats->samples, percentile);
    uint64_t cumulative_samples = 0;
    for (size_t bucket_index = 0; bucket_index < USFS_STATUS_LATENCY_BUCKETS; ++bucket_index)
    {
        cumulative_samples += latency_stats->buckets[bucket_index];
        if (cumulative_samples >= target_sample)
            return usfs_status_latency_bounds_ms[bucket_index];
    }

    return UINT32_MAX;
}

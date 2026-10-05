// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_GCOV_H
#define USFS_GCOV_H

#ifdef _KERNEL
    #include <sys/inttypes.h>
#else
    #include <stdint.h>
#endif

#define USFS_GCOV_IOC_INFO     0x55530100
#define USFS_GCOV_IOC_SNAPSHOT 0x55530101
#define USFS_GCOV_IOC_RESET    0x55530102

#define USFS_GCOV_ABI_VERSION  1u
#define USFS_GCOV_VERSION      0x4233332au /* GCC 13.3.0: "B33*" */
#define USFS_GCOV_MAGIC        0x55534647u /* "USFG" in native byte order */
#define USFS_GCOV_FLAG_ARCS    0x00000001u
#define USFS_GCOV_MAX_SNAPSHOT (256u * 1024u)
#define USFS_GCOV_FILENAME_MAX 128u

struct usfs_gcov_info
{
    uint32_t abi_version;
    uint32_t gcov_version;
    uint32_t unit_count;
    uint32_t flags;
    uint32_t snapshot_size;
    uint32_t reserved;
};

struct usfs_gcov_snapshot_request
{
    uint32_t abi_version;
    uint32_t capacity;
    uint32_t size;
    uint32_t reserved;
    uint64_t user_buffer;
};

struct usfs_gcov_container_header
{
    uint32_t magic;
    uint32_t abi_version;
    uint32_t gcov_version;
    uint32_t unit_count;
    uint32_t total_size;
    uint32_t reserved;
};

struct usfs_gcov_unit_header
{
    uint32_t filename_length;
    uint32_t gcda_length;
    uint32_t version;
    uint32_t stamp;
    uint32_t checksum;
    uint32_t reserved;
};

#define USFS_GCOV_ALIGN8(n) (((n) + 7u) & ~7u)

#endif

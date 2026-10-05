// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"

#include <sys/err_rec.h>

#include "usfs_errids.h"

enum
{
    ERROR_RECORD_DETAIL_BYTES = 28,
    NO_ERROR_RECORD_ID = 0
};

typedef ERR_REC (ERROR_RECORD_DETAIL_BYTES) usfs_error_record_t;

static uint32_t select_error_record_id (const uint32_t fault)
{
    switch (fault)
    {
        case USFS_FAULT_TIMEOUT:
            return ERRID_USFS_TIMEOUT;
        case USFS_FAULT_PROTOCOL:
            return ERRID_USFS_PROTOCOL;
        case USFS_FAULT_DAEMON_LOST:
            return ERRID_USFS_DAEMON_LOST;
        case USFS_FAULT_FORCED_RECOVERY:
            return ERRID_USFS_FORCE_RECOVERY;
        case USFS_FAULT_CLEANUP_REQUIRED:
            return ERRID_USFS_CLEANUP_REQ;
        default:
            return NO_ERROR_RECORD_ID;
    }
}

static void append_error_detail (usfs_error_record_t * record, unsigned * offset, const void * value, const unsigned value_size)
{
    memcpy (record->detail_data + *offset, value, value_size);
    *offset += value_size;
}

void usfs_ras_report (
    const uint32_t fault,
    const int error,
    const int channel,
    const uint16_t opcode,
    const uint64_t request_id,
    const uint32_t mount_count
)
{
    const uint32_t error_id = select_error_record_id (fault);
    if (error_id == NO_ERROR_RECORD_ID)
        return;

    usfs_error_record_t record;

    memset (&record, 0, sizeof (record));
    record.error_id = error_id;
    (void)strncpy (record.resource_name, "usfs0", sizeof (record.resource_name) - 1u);

    unsigned detail_offset = 0;
    const uint32_t opcode_value = opcode;

    append_error_detail (&record, &detail_offset, &fault, sizeof (fault));
    append_error_detail (&record, &detail_offset, &error, sizeof (error));
    append_error_detail (&record, &detail_offset, &channel, sizeof (channel));
    append_error_detail (&record, &detail_offset, &opcode_value, sizeof (opcode_value));
    append_error_detail (&record, &detail_offset, &request_id, sizeof (request_id));
    append_error_detail (&record, &detail_offset, &mount_count, sizeof (mount_count));

    errsave ((struct err_rec *)&record, (int)sizeof (record));
}

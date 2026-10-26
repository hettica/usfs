// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include "tap.h"

// Keep the fixture's runtime separate from compiler-generated coverage hooks.
#define __gcov_init test_gcov_init
#define __gcov_exit test_gcov_exit
#define __gcov_merge_add test_gcov_merge_add
#include "../../../src/kernel/instrumentation/coverage/runtime.c"
#undef __gcov_init
#undef __gcov_exit
#undef __gcov_merge_add
#undef malloc
#undef free

heapaddr_t kernel_heap;
static int allocation_balance;

void * xmalloc (uint32long64_t size, uint alignment, heapaddr_t heap)
{
    (void)alignment;
    (void)heap;

    void * allocation = malloc (size);
    if (allocation != NULL)
        allocation_balance++;

    return allocation;
}

int xmfree (void * allocation, heapaddr_t heap)
{
    (void)heap;

    free (allocation);
    allocation_balance--;

    return 0;
}

void usfs_gcov_register_all (void)
{
}

unsigned usfs_gcov_expected_units (void)
{
    return 1;
}

static void verify_snapshot (struct tap_state * tap, struct gcov_info * unit)
{
    unsigned expected_size = 0;
    const int size_rc = usfs_gcov_snapshot_size (&expected_size);
    if (size_rc != 0)
        abort ();

    void * snapshot = NULL;
    unsigned snapshot_size = 0;
    const int rc = usfs_gcov_snapshot_create (&snapshot, &snapshot_size);
    if (rc != 0)
        abort ();

    const struct usfs_gcov_container_header * header = snapshot;
    const struct usfs_gcov_unit_header * record = (const struct usfs_gcov_unit_header *)(header + 1);
    const char * filename = (const char *)(record + 1);

    tap_ok (tap, snapshot_size == expected_size && header->total_size == expected_size &&
                 header->unit_count == 1 && allocation_balance == 1,
            "snapshot allocation and serialized size agree");
    tap_ok (tap, record->filename_length == sizeof ("fixture.gcda") &&
                 strcmp (filename, "fixture.gcda") == 0 && record->version == unit->version,
            "snapshot initializes the filename length and copies its terminating NUL");

    const unsigned char * gcda = (const unsigned char *)filename + USFS_GCOV_ALIGN8 (record->filename_length);
    gcov_unsigned_t magic = 0;

    memcpy (&magic, gcda, sizeof (magic));

    tap_ok (tap, magic == GCOV_DATA_MAGIC &&
                 gcda + USFS_GCOV_ALIGN8 (record->gcda_length) == (unsigned char *)snapshot + snapshot_size,
            "snapshot initializes the gcda length and ends at the allocation boundary");

    usfs_gcov_snapshot_destroy (snapshot);

    tap_ok (tap, allocation_balance == 0, "snapshot destruction releases its allocation");
}

int main (void)
{
    struct tap_state tap;

    tap_plan (&tap, 7);

    gcov_type counters[] = { 7, 11 };
    struct gcov_info unit = { 0 };
    const struct gcov_fn_info function = {
        .key = &unit,
        .ident = 1,
        .ctrs = { { .num = sizeof (counters) / sizeof (counters[0]), .values = counters } }
    };
    const struct gcov_fn_info * functions[] = { &function };

    unit.version = USFS_GCOV_VERSION;
    unit.filename = "/build/fixture.gcda";
    unit.merge[GCOV_ARC_COUNTER_INDEX] = test_gcov_merge_add;
    unit.n_functions = sizeof (functions) / sizeof (functions[0]);
    unit.functions = functions;

    test_gcov_init (&unit);

    tap_ok (&tap, usfs_gcov_runtime_initialize (1) == 0, "valid metadata initializes the coverage runtime");

    verify_snapshot (&tap, &unit);

    tap_ok (&tap, usfs_gcov_reset_counters () == 0 && counters[0] == 0 && counters[1] == 0,
            "exported counters can be reset without changing metadata");

    unit.filename = "invalid.txt";
    void * snapshot = NULL;
    unsigned snapshot_size = 0;

    tap_ok (&tap, usfs_gcov_snapshot_create (&snapshot, &snapshot_size) == EINVAL &&
                 snapshot == NULL && snapshot_size == 0 && allocation_balance == 0,
            "invalid filenames fail before allocation or publication");

    usfs_gcov_runtime_shutdown ();

    return tap_finish (&tap);
}

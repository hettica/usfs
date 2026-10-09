/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * USFS instrumented-kernel gcov runtime for GCC 13.3.0 on AIX 7.2.
 *
 * The gcda layout and metadata structures implemented here follow GCC
 * 13.3.0's gcov-io.h and libgcov-driver.c. GCC's runtime sources are GPLv3+
 * with the GCC Runtime Library Exception 3.1. This file is a project-specific
 * arcs-only implementation; it does not link or embed libgcov.
 */

#include "definitions.h"
#include "runtime.h"

typedef unsigned gcov_unsigned_t;
typedef long long gcov_type;
typedef void (*gcov_merge_fn) (gcov_type *, gcov_unsigned_t);

#define GCOV_COUNTERS                 8
#define GCOV_DATA_MAGIC               0x67636461U
#define GCOV_TAG_FUNCTION             0x01000000U
#define GCOV_TAG_FUNCTION_LENGTH      12U
#define GCOV_TAG_COUNTER_BASE         0x01a10000U
#define GCOV_COUNTER_SERIALIZED_BYTES 8U
#define GCOV_COUNTER_KIND_SHIFT       17
#define GCOV_TAG_COUNTER_LENGTH(n)    ((n)*GCOV_COUNTER_SERIALIZED_BYTES)
#define GCOV_TAG_FOR_COUNTER(n)       (GCOV_TAG_COUNTER_BASE + ((n) << GCOV_COUNTER_KIND_SHIFT))
#define GCOV_MAX_UNITS                32

enum
{
    GCOV_ARC_COUNTER_INDEX = 0,
    GCOV_COUNTER_HIGH_WORD_SHIFT = 32,
    GCOV_END_TAG = 0,
    GCOV_ABSENT_FUNCTION_LENGTH = 0,
    SNAPSHOT_ALLOCATION_ALIGNMENT = 4
};

struct gcov_ctr_info
{
    gcov_unsigned_t num; // Number of counters in the GCC metadata record.
    gcov_type * values;  // Compiler-owned counter storage.
};

struct gcov_info;

struct gcov_fn_info
{
    const struct gcov_info * key;    // Owning compilation unit.
    gcov_unsigned_t ident;           // Compiler-assigned function identifier.
    gcov_unsigned_t lineno_checksum; // GCC source-line checksum.
    gcov_unsigned_t cfg_checksum;    // GCC control-flow checksum.
    struct gcov_ctr_info ctrs[1];    // Arc counter metadata in GCC ABI order.
};

struct gcov_info
{
    gcov_unsigned_t version;                       // GCC coverage format version.
    struct gcov_info * next;                       // GCC registration link retained for ABI layout.
    gcov_unsigned_t stamp;                         // Compilation timestamp identity.
    gcov_unsigned_t checksum;                      // Compilation unit checksum.
    const char * filename;                         // Compiler-owned gcda pathname.
    gcov_merge_fn merge[GCOV_COUNTERS];            // Merge function for each counter kind.
    gcov_unsigned_t n_functions;                   // Number of function metadata entries.
    const struct gcov_fn_info * const * functions; // Compiler-owned function table.
};

struct gcov_writer
{
    unsigned char * output_buffer; // Output storage, or NULL while calculating the serialized size.
    unsigned capacity_bytes;       // Available output bytes.
    unsigned used_bytes;           // Bytes required by serialized records.
    int has_overflow;              // A write exceeded the available output storage.
};

static struct gcov_info * registered_units[GCOV_MAX_UNITS];
static unsigned registered_unit_count;
static int registration_error;
static int is_runtime_ready;

static int has_word_capacity (const struct gcov_writer * writer)
{
    if (writer->used_bytes > writer->capacity_bytes)
        return false;

    return writer->capacity_bytes - writer->used_bytes >= sizeof (gcov_unsigned_t);
}

static void write_word (struct gcov_writer * writer, const gcov_unsigned_t value)
{
    if (has_word_capacity (writer))
    {
        memcpy (writer->output_buffer + writer->used_bytes, &value, sizeof (value));
    }
    else if (writer->output_buffer != NULL)
    {
        writer->has_overflow = true;
    }

    writer->used_bytes += sizeof (value);
}

static void write_counter (struct gcov_writer * writer, gcov_type * value)
{
    const unsigned long counter_bits = (unsigned long)fetch_and_addlp ((atomic_l)value, 0);

    write_word (writer, (gcov_unsigned_t)counter_bits);
    write_word (writer, (gcov_unsigned_t)(counter_bits >> GCOV_COUNTER_HIGH_WORD_SHIFT));
}

static const char * get_filename_basename (const char * filename)
{
    const char * basename = filename;

    if (filename == NULL)
        return NULL;

    for (const char * cursor = filename; *cursor != '\0'; cursor++)
    {
        if (*cursor == '/')
            basename = cursor + 1;
    }

    return basename;
}

static int is_filename_character (const char character)
{
    if (character >= 'a' && character <= 'z')
        return true;

    if (character >= 'A' && character <= 'Z')
        return true;

    if (character >= '0' && character <= '9')
        return true;

    if (character == '_')
        return true;

    if (character == '-')
        return true;

    return character == '.';
}

static int validate_filename_extension (const char * filename, const unsigned length)
{
    static const char extension[] = ".gcda";
    const unsigned extension_length = sizeof (extension) - 1;

    if (length <= extension_length)
        return EINVAL;

    if (strcmp (filename + length - extension_length, extension) != 0)
        return EINVAL;

    return 0;
}

static int validate_unit_filename (const char * name, unsigned * length)
{
    if (name == NULL)
        return EINVAL;

    if (name[0] == '\0')
        return EINVAL;

    for (unsigned character_index = 0; character_index < USFS_GCOV_FILENAME_MAX; character_index++)
    {
        const char character = name[character_index];

        if (character == '\0')
        {
            const int rc = validate_filename_extension (name, character_index);
            if (rc != 0)
                return rc;

            *length = character_index + 1;

            return 0;
        }

        if (!is_filename_character (character))
            return EINVAL;
    }

    return ENAMETOOLONG;
}

void __gcov_init (struct gcov_info * info)
{
    if (info == NULL)
    {
        registration_error = EINVAL;

        return;
    }

    if (is_runtime_ready)
    {
        registration_error = EINVAL;

        return;
    }

    for (unsigned unit_index = 0; unit_index < registered_unit_count; unit_index++)
    {
        if (registered_units[unit_index] == info)
        {
            registration_error = EEXIST;

            return;
        }
    }

    if (registered_unit_count >= GCOV_MAX_UNITS)
    {
        registration_error = E2BIG;

        return;
    }

    registered_units[registered_unit_count++] = info;
}

void __gcov_exit (void)
{
}

void __gcov_merge_add (gcov_type * counters, const gcov_unsigned_t count)
{
    (void)counters;
    (void)count;
}

static int validate_unit_metadata (const struct gcov_info * info, const unsigned unit_index)
{
    if (info == NULL)
        return EINVAL;

    if (info->version != USFS_GCOV_VERSION)
        return EINVAL;

    if (info->merge[GCOV_ARC_COUNTER_INDEX] != __gcov_merge_add)
        return EINVAL;

    if (info->n_functions == 0)
        return EINVAL;

    if (info->functions == NULL)
        return EINVAL;

    for (unsigned counter_kind = 1; counter_kind < GCOV_COUNTERS; counter_kind++)
        if (info->merge[counter_kind] != NULL)
            return ENOTSUP;

    const char * unit_filename = get_filename_basename (info->filename);
    unsigned name_length;

    if (validate_unit_filename (unit_filename, &name_length) != 0)
        return EINVAL;

    (void)name_length;

    for (unsigned prior_unit_index = 0; prior_unit_index < unit_index; prior_unit_index++)
        if (strcmp (unit_filename, get_filename_basename (registered_units[prior_unit_index]->filename)) == 0)
            return EEXIST;

    return 0;
}

static void write_function_record (struct gcov_writer * writer, const struct gcov_info * info, const struct gcov_fn_info * function)
{
    write_word (writer, GCOV_TAG_FUNCTION);

    if (function == NULL)
    {
        write_word (writer, GCOV_ABSENT_FUNCTION_LENGTH);

        return;
    }

    if (function->key != info)
    {
        write_word (writer, GCOV_ABSENT_FUNCTION_LENGTH);

        return;
    }

    write_word (writer, GCOV_TAG_FUNCTION_LENGTH);
    write_word (writer, function->ident);
    write_word (writer, function->lineno_checksum);
    write_word (writer, function->cfg_checksum);

    const struct gcov_ctr_info * counter = function->ctrs;

    write_word (writer, GCOV_TAG_FOR_COUNTER (GCOV_ARC_COUNTER_INDEX));
    write_word (writer, GCOV_TAG_COUNTER_LENGTH (counter->num));

    for (unsigned value_index = 0; value_index < counter->num; value_index++)
        write_counter (writer, &counter->values[value_index]);
}

static int write_unit_records (const struct gcov_info * info, void * output_buffer, const unsigned capacity_bytes, unsigned * size_out)
{
    struct gcov_writer writer = { 0 };

    writer.output_buffer = output_buffer;
    writer.capacity_bytes = capacity_bytes;

    write_word (&writer, GCOV_DATA_MAGIC);
    write_word (&writer, info->version);
    write_word (&writer, info->stamp);
    write_word (&writer, info->checksum);

    for (unsigned function_index = 0; function_index < info->n_functions; function_index++)
        write_function_record (&writer, info, info->functions[function_index]);

    write_word (&writer, GCOV_END_TAG);
    *size_out = writer.used_bytes;

    if (output_buffer == NULL)
        return 0;

    if (writer.has_overflow)
        return ENOSPC;

    if (writer.used_bytes > capacity_bytes)
        return ENOSPC;

    return 0;
}

int usfs_gcov_runtime_initialize (const unsigned expected_units)
{
    if (registration_error != 0)
        return EINVAL;

    if (expected_units == 0)
        return EINVAL;

    if (expected_units > GCOV_MAX_UNITS)
        return EINVAL;

    if (registered_unit_count != expected_units)
        return EINVAL;

    for (unsigned unit_index = 0; unit_index < registered_unit_count; unit_index++)
    {
        const int rc = validate_unit_metadata (registered_units[unit_index], unit_index);
        if (rc != 0)
            return rc;
    }

    is_runtime_ready = true;

    unsigned snapshot_size;

    const int rc = usfs_gcov_snapshot_size (&snapshot_size);
    if (rc != 0)
    {
        is_runtime_ready = false;

        return rc;
    }

    if (snapshot_size > USFS_GCOV_MAX_SNAPSHOT)
    {
        is_runtime_ready = false;

        return EFBIG;
    }

    return 0;
}

int usfs_gcov_runtime_prepare_export (void)
{
    if (is_runtime_ready)
        return 0;

    if (registered_unit_count == 0 && registration_error == 0)
        usfs_gcov_register_all ();

    return usfs_gcov_runtime_initialize (usfs_gcov_expected_units ());
}

void usfs_gcov_runtime_shutdown (void)
{
    memset (registered_units, 0, sizeof (registered_units));
    registered_unit_count = 0;
    registration_error = 0;
    is_runtime_ready = false;
}

unsigned usfs_gcov_unit_count (void)
{
    return is_runtime_ready ? registered_unit_count : 0;
}

int usfs_gcov_snapshot_size (unsigned * size_out)
{
    if (!is_runtime_ready)
        return EINVAL;

    if (size_out == NULL)
        return EINVAL;

    unsigned snapshot_size = sizeof (struct usfs_gcov_container_header);

    for (unsigned unit_index = 0; unit_index < registered_unit_count; unit_index++)
    {
        unsigned filename_length;

        int rc = validate_unit_filename (get_filename_basename (registered_units[unit_index]->filename), &filename_length);
        if (rc != 0)
            return rc;

        unsigned gcda_size;

        rc = write_unit_records (registered_units[unit_index], NULL, 0, &gcda_size);
        if (rc != 0)
            return rc;

        snapshot_size += sizeof (struct usfs_gcov_unit_header);
        snapshot_size += USFS_GCOV_ALIGN8 (filename_length);
        snapshot_size += USFS_GCOV_ALIGN8 (gcda_size);

        if (snapshot_size > USFS_GCOV_MAX_SNAPSHOT)
            return EFBIG;
    }

    *size_out = snapshot_size;

    return 0;
}

struct snapshot_writer
{
    struct usfs_gcov_container_header * header; // Start of the zero-filled snapshot allocation.
    unsigned size_bytes;                        // Total allocation size in bytes.
    unsigned char * cursor;                     // Next unit record to serialize.
};

static int write_snapshot_unit (struct snapshot_writer * writer, const struct gcov_info * info)
{
    struct usfs_gcov_unit_header * unit = (struct usfs_gcov_unit_header *)writer->cursor;
    const char * filename = get_filename_basename (info->filename);
    unsigned filename_length;

    writer->cursor += sizeof (*unit);
    (void)validate_unit_filename (filename, &filename_length);
    unit->filename_length = filename_length;
    unit->version = info->version;
    unit->stamp = info->stamp;
    unit->checksum = info->checksum;
    memcpy (writer->cursor, filename, filename_length);
    writer->cursor += USFS_GCOV_ALIGN8 (filename_length);

    const unsigned remaining_bytes = writer->size_bytes - (unsigned)(writer->cursor - (unsigned char *)writer->header);
    unsigned gcda_size;

    const int rc = write_unit_records (info, writer->cursor, remaining_bytes, &gcda_size);
    if (rc != 0)
        return rc;

    unit->gcda_length = gcda_size;
    writer->cursor += USFS_GCOV_ALIGN8 (gcda_size);

    return 0;
}

static int write_snapshot_units (struct usfs_gcov_container_header * header, const unsigned snapshot_size)
{
    struct snapshot_writer writer = { .header = header, .size_bytes = snapshot_size, .cursor = (unsigned char *)(header + 1) };

    for (unsigned unit_index = 0; unit_index < registered_unit_count; unit_index++)
    {
        const int rc = write_snapshot_unit (&writer, registered_units[unit_index]);
        if (rc != 0)
            return rc;
    }

    if ((unsigned)(writer.cursor - (unsigned char *)header) != snapshot_size)
        return EIO;

    return 0;
}

int usfs_gcov_snapshot_create (void ** output_buffer, unsigned * size_out)
{
    if (output_buffer == NULL)
        return EINVAL;

    if (size_out == NULL)
        return EINVAL;

    unsigned snapshot_size;

    int rc = usfs_gcov_snapshot_size (&snapshot_size);
    if (rc != 0)
        return rc;

    struct usfs_gcov_container_header * header = xmalloc (snapshot_size, SNAPSHOT_ALLOCATION_ALIGNMENT, kernel_heap);
    if (header == NULL)
        return ENOMEM;

    memset (header, 0, snapshot_size);
    header->magic = USFS_GCOV_MAGIC;
    header->abi_version = USFS_GCOV_ABI_VERSION;
    header->gcov_version = USFS_GCOV_VERSION;
    header->unit_count = registered_unit_count;
    header->total_size = snapshot_size;

    rc = write_snapshot_units (header, snapshot_size);
    if (rc != 0)
    {
        xmfree (header, kernel_heap);

        return rc;
    }

    *output_buffer = header;
    *size_out = snapshot_size;

    return 0;
}

void usfs_gcov_snapshot_destroy (void * output_buffer)
{
    if (output_buffer != NULL)
        xmfree (output_buffer, kernel_heap);
}

static void reset_function_counters (const struct gcov_info * info, const struct gcov_fn_info * function)
{
    if (function == NULL)
        return;

    if (function->key != info)
        return;

    const struct gcov_ctr_info * counter = function->ctrs;

    for (unsigned value_index = 0; value_index < counter->num; value_index++)
    {
        long previous_value;

        do
        {
            previous_value = fetch_and_addlp ((atomic_l)&counter->values[value_index], 0);
        }
        while (!compare_and_swaplp ((atomic_l)&counter->values[value_index], &previous_value, 0));
    }
}

int usfs_gcov_reset_counters (void)
{
    if (!is_runtime_ready)
        return EINVAL;

    for (unsigned unit_index = 0; unit_index < registered_unit_count; unit_index++)
    {
        const struct gcov_info * info = registered_units[unit_index];

        for (unsigned function_index = 0; function_index < info->n_functions; function_index++)
            reset_function_counters (info, info->functions[function_index]);
    }

    return 0;
}

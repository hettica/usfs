/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Test-only programmable USFS daemon.  This deliberately bypasses libfuse so
 * contract tests observe the kernel protocol without translating it through
 * client-library behavior.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mntctl.h>
#include <sys/stat.h>
#include <sys/vmount.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include "usfs_file.h"
#include "usfs_proto.h"
#include "usfs_validate.h"

#define DEVICE    "/dev/usfs0"
#define MAX_RULES 64

enum action
{
    ACTION_SUCCESS,
    ACTION_ERROR,
    ACTION_WRONG_VERSION,
    ACTION_WRONG_OPCODE,
    ACTION_WRONG_UNIQUE,
    ACTION_TRUNCATED,
    ACTION_OVERSIZED,
    ACTION_WRONG_BODY,
    ACTION_DELAY,
    ACTION_DISCONNECT,
    ACTION_DUPLICATE,
    ACTION_REORDERED,
    ACTION_INVALID_NODEID,
    ACTION_INVALID_ATTR,
    ACTION_INVALID_PARENT,
    ACTION_INVALID_WRITE_COUNT,
    ACTION_INVALID_WRITE_OFFSET,
    ACTION_INVALID_READ_COUNT,
    ACTION_INVALID_READDIR_COUNT,
    ACTION_INVALID_READDIR_RECORD,
    ACTION_INVALID_STATFS,
    ACTION_SHORT_BODY,
    ACTION_INVALID_OPEN_PAD,
    ACTION_REPLY_DISCONNECT,
    ACTION_SHORT_WRITE,
    ACTION_RANDOM_BYTES,
    ACTION_HOLD,
    ACTION_REPLY_COPY_FAULT
};

struct rule
{
    uint16_t opcode;     // Request opcode matched by this rule.
    enum action action;  // Reply behavior selected on the requested occurrence.
    int error;           // Error code or delay in milliseconds, according to action.
    unsigned occurrence; // One-based matching request occurrence to affect.
    unsigned seen;       // Matching requests observed so far.
};

static struct rule rules[MAX_RULES];
static size_t rule_count;
static volatile sig_atomic_t stopping;
static FILE * request_log;
static const char * barrier_directory;
static int full_reads;
static int wide_readdir;
static int force_sync_rules;
static uint64_t scenario_file_size = 13;

struct test_node
{
    char name[USFS_MAX_NAME];            // Backend entry name within its parent.
    uint64_t node_id;                    // Node identifier used in protocol replies.
    uint64_t parent_id;                  // Parent node identifier for backend lookup.
    uint32_t mode;                       // File type and permission bits.
    struct usfs_attr initial_attributes; // Attributes saved when the node was created.
};

static struct test_node nodes[32];
static size_t node_count;

/* Wire ownership is independent of backend names, including removed names. */
static struct
{
    uint64_t node_id; // Node identifier whose lookup references are owned.
    uint64_t count;   // Outstanding lookup reference count for the node.
} lookup_owners[128];
static struct
{
    uint64_t file_handle; // Open file handle owned by the kernel.
    int live;             // Whether the handle has not yet been released.
} handle_owners[128];
static unsigned handles_created;
static unsigned handles_released;
static unsigned ownership_errors;

static int handle_ownership (const uint64_t file_handle, const int acquire)
{
    size_t empty_slot = 128;
    for (size_t owner_index = 0; owner_index < 128; ++owner_index)
    {
        if (!handle_owners[owner_index].live)
        {
            empty_slot = owner_index;
            continue;
        }
        if (handle_owners[owner_index].file_handle != file_handle)
            continue;
        if (acquire)
        {
            ++ownership_errors;
            return EINVAL;
        }
        handle_owners[owner_index].live = 0;
        ++handles_released;
        return 0;
    }
    if (!acquire || empty_slot == 128)
    {
        ++ownership_errors;
        return EINVAL;
    }
    handle_owners[empty_slot].file_handle = file_handle;
    handle_owners[empty_slot].live = 1;
    ++handles_created;
    return 0;
}

static void close_ownership (void)
{
    unsigned closed_handles = 0;
    uint64_t closed_lookup_references = 0;
    for (size_t owner_index = 0; owner_index < 128; ++owner_index)
    {
        if (handle_owners[owner_index].live)
        {
            ++closed_handles;
            handle_owners[owner_index].live = 0;
        }
        closed_lookup_references += lookup_owners[owner_index].count;
        lookup_owners[owner_index].count = 0;
    }
    fprintf (request_log, "OWNERSHIP created=%u released=%u closed=%u lookup_closed=%llu errors=%u\n", handles_created, handles_released, closed_handles, (unsigned long long)closed_lookup_references, ownership_errors);
}

static int lookup_ownership (const uint64_t node_id, const uint64_t count, const int acquire)
{
    size_t empty_slot = 128;
    for (size_t owner_index = 0; owner_index < 128; ++owner_index)
    {
        if (lookup_owners[owner_index].count == 0)
        {
            empty_slot = owner_index;
            continue;
        }
        if (lookup_owners[owner_index].node_id != node_id)
            continue;
        if (acquire)
        {
            if (count > UINT64_MAX - lookup_owners[owner_index].count)
                return EOVERFLOW;
            lookup_owners[owner_index].count += count;
        }
        else
        {
            struct usfs_forget_in body = { count };
            if (!usfs_forget_in_valid (&body, lookup_owners[owner_index].count))
                return EINVAL;
            lookup_owners[owner_index].count -= count;
        }
        return 0;
    }
    if (!acquire || count == 0)
        return EINVAL;

    if (empty_slot == 128)
        return ENOSPC;
    lookup_owners[empty_slot].node_id = node_id;
    lookup_owners[empty_slot].count = count;
    return 0;
}

static struct test_node * find_node_name (const uint64_t parent_id, const char * name)
{
    for (size_t node_index = 0; node_index < node_count; ++node_index)
        if (nodes[node_index].parent_id == parent_id &&
            strcmp (nodes[node_index].name, name) == 0)
            return &nodes[node_index];

    return NULL;
}

static struct test_node * find_node_id (const uint64_t node_id)
{
    for (size_t node_index = 0; node_index < node_count; ++node_index)
        if (nodes[node_index].node_id == node_id)
            return &nodes[node_index];

    return NULL;
}

static int add_node (const char * name, const uint64_t node_id, const uint64_t parent_id, const uint32_t mode)
{
    if (strlen (name) >= USFS_MAX_NAME || node_count == 32)
        return -1;
    strcpy (nodes[node_count].name, name);
    memset (&nodes[node_count].initial_attributes, 0, sizeof (nodes[node_count].initial_attributes));
    nodes[node_count].node_id = node_id;
    nodes[node_count].parent_id = parent_id;
    nodes[node_count].mode = mode;
    ++node_count;
    return 0;
}

static void remove_node (const uint64_t parent_id, const char * name)
{
    struct test_node * node = find_node_name (parent_id, name);

    if (node == NULL)
        return;
    *node = nodes[node_count - 1];
    --node_count;
}

static void on_signal (const int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void align_word (char ** field_cursor)
{
    while ((uintptr_t)*field_cursor % 4u != 0)
        *field_cursor += 1;
}

static void add_field (struct vmount * mount_record, char ** field_cursor, const int field_index, const char * field_text)
{
    align_word (field_cursor);
    mount_record->vmt_data[field_index].vmt_off = (int)(*field_cursor - (char *)mount_record);
    mount_record->vmt_data[field_index].vmt_size = (int)strlen (field_text) + 1;
    strcpy (*field_cursor, field_text);
    *field_cursor += strlen (field_text) + 1;
}

static void corrupt_mount_record (struct vmount * mount_record, const char * corruption)
{
    if (corruption != NULL &&
        strcmp (corruption, "negative-info-offset") == 0)
        mount_record->vmt_data[VMT_INFO].vmt_off = -1;
    else if (corruption != NULL && strcmp (corruption, "past-end-info") == 0)
    {
        mount_record->vmt_data[VMT_INFO].vmt_off =
            (short)(mount_record->vmt_length - 1);
        mount_record->vmt_data[VMT_INFO].vmt_size = 16;
    }
    else if (corruption != NULL && strcmp (corruption, "unterminated-info") == 0)
    {
        char * info_text = (char *)mount_record +
                           mount_record->vmt_data[VMT_INFO].vmt_off;
        info_text[mount_record->vmt_data[VMT_INFO].vmt_size - 1] = 'x';
    }
    else if (corruption != NULL && strcmp (corruption, "invalid-rw") == 0)
    {
        char * info_text = (char *)mount_record +
                           mount_record->vmt_data[VMT_INFO].vmt_off;
        info_text[mount_record->vmt_data[VMT_INFO].vmt_size - 2] = '2';
    }
}

static struct vmount * prepare_mount_record (char * buffer, const struct usfs_dev_info * device_info, const char * resolved_mount_path, char * options, const char * corruption)
{
    struct vmount * mount_record = (struct vmount *)buffer;
    char * field_cursor = buffer + sizeof (*mount_record);
    mount_record->vmt_revision = VMT_REVISION;
    mount_record->vmt_gfstype = device_info->fs_type;
    /* Deliberately disagree with rw=1 to verify native mount restrictions. */
    if (getenv ("USFS_SCENARIO_READONLY_FLAGS") != NULL)
        mount_record->vmt_flags |= MNT_READONLY;
    if (getenv ("USFS_SCENARIO_READONLY_POLICY") != NULL)
        options[strlen (options) - 1] = '0';
    if (corruption != NULL && strcmp (corruption, "missing-rw") == 0)
        *strstr (options, ",rw=") = '\0';
    add_field (mount_record, &field_cursor, VMT_OBJECT, "usfs-contract");
    add_field (mount_record, &field_cursor, VMT_STUB, resolved_mount_path);
    add_field (mount_record, &field_cursor, VMT_HOST, "localhost");
    add_field (mount_record, &field_cursor, VMT_HOSTNAME, "localhost.localdomain");
    add_field (mount_record, &field_cursor, VMT_INFO, options);
    add_field (mount_record, &field_cursor, VMT_ARGS, options);
    align_word (&field_cursor);
    mount_record->vmt_length = (int)(field_cursor - buffer);
    corrupt_mount_record (mount_record, corruption);
    return mount_record;
}

static int mount_channel (const int device_descriptor, const struct usfs_dev_info * device_info, const char * mountpoint)
{
    char resolved_mount_path[PATH_MAX];
    char options[128];

    if (realpath (mountpoint, resolved_mount_path) == NULL)
        return -1;

    const char * corruption = getenv ("USFS_SCENARIO_VMOUNT_CORRUPTION");
    if (corruption != NULL &&
        strcmp (corruption, "overflow-cookie") == 0)
        snprintf (options, sizeof (options), "fd=%d,chan=%d,cookie=1%016llx,rw=1", device_descriptor, (int)device_info->channel, (unsigned long long)device_info->cookie);
    else
        snprintf (options, sizeof (options), "fd=%d,chan=%d,cookie=%016llx,rw=1", device_descriptor, (int)device_info->channel, (unsigned long long)device_info->cookie);
    char * buffer = calloc (1, 4096);
    if (buffer == NULL)
        return -1;
    struct vmount * mount_record = prepare_mount_record (buffer, device_info, resolved_mount_path, options, corruption);
    const int rc = vmount (mount_record, mount_record->vmt_length);
    free (buffer);
    return rc;
}

static void run_mount_child (const int device_descriptor, const struct usfs_dev_info * device_info, const char * mountpoint, const int * start_gate)
{
    char command;
    close (start_gate[1]);
    if (read (start_gate[0], &command, 1) != 1)
        _exit (2);
    close (start_gate[0]);
    const int rc = mount_channel (device_descriptor, device_info, mountpoint);
    _exit (rc == 0 ? 0 : errno == EBUSY ? 1
                                        : 2);
}

static int mount_channel_pair (const int device_descriptor, const struct usfs_dev_info * device_info, const char * first_mountpoint, const char * second_mountpoint)
{
    int start_gate[2];
    int child_statuses[2] = { -1, -1 };
    int started_children = 0;
    int rc = -1;
    pid_t child_processes[2];
    if (pipe (start_gate) != 0)
        return -1;
    for (int child_index = 0; child_index < 2; ++child_index)
    {
        child_processes[child_index] = fork ();
        if (child_processes[child_index] == 0)
        {
            run_mount_child (device_descriptor, device_info, child_index == 0 ? first_mountpoint : second_mountpoint, start_gate);
        }
        if (child_processes[child_index] < 0)
            break;
        ++started_children;
    }
    close (start_gate[0]);
    if (started_children == 2 && write (start_gate[1], "xx", 2) != 2)
        started_children = -2;
    close (start_gate[1]);
    for (int child_index = 0; child_index < (started_children < 0 ? 2 : started_children); ++child_index)
    {
        int status;
        if (waitpid (child_processes[child_index], &status, 0) == child_processes[child_index] && WIFEXITED (status))
            child_statuses[child_index] = WEXITSTATUS (status);
    }
    if (started_children == 2 && ((child_statuses[0] == 0 && child_statuses[1] == 1) ||
                                  (child_statuses[1] == 0 && child_statuses[0] == 1)))
    {
        rc = 0;
        printf ("MOUNT_RACE_WINNER %s\n", child_statuses[0] == 0 ? first_mountpoint : second_mountpoint);
    }
    if (rc != 0)
        fprintf (stderr, "mount race results=%d,%d\n", child_statuses[0], child_statuses[1]);
    return rc;
}

static int opcode_from_name (const char * name, uint16_t * opcode)
{
    static const char * const names[] = {
        NULL,
        "LOOKUP",
        "GETATTR",
        "OPEN",
        "READ",
        "RELEASE",
        "READDIR",
        "READLINK",
        "STATFS",
        "CREATE",
        "MKDIR",
        "UNLINK",
        "RMDIR",
        "RENAME",
        "SYMLINK",
        "LINK",
        "SETATTR",
        "WRITE",
        "FLUSH",
        "FSYNC",
        "SYNCFS",
        "CREATE_ATTR",
        "FORGET",
        "FID",
        "VGET"
    };

    for (unsigned opcode_index = 1; opcode_index <= USFS_OP_VGET; ++opcode_index)
    {
        if (strcmp (name, names[opcode_index]) == 0)
        {
            *opcode = (uint16_t)opcode_index;
            return 0;
        }
    }
    return -1;
}

static int action_from_name (const char * name, enum action * action)
{
    static const struct
    {
        const char * name;  // Action spelling accepted in a scenario rule.
        enum action action; // Corresponding reply behavior.
    } action_names[] = {
        { "success", ACTION_SUCCESS },
        { "error", ACTION_ERROR },
        { "wrong-version", ACTION_WRONG_VERSION },
        { "wrong-opcode", ACTION_WRONG_OPCODE },
        { "wrong-unique", ACTION_WRONG_UNIQUE },
        { "truncated", ACTION_TRUNCATED },
        { "oversized", ACTION_OVERSIZED },
        { "wrong-body", ACTION_WRONG_BODY },
        { "delay", ACTION_DELAY },
        { "disconnect", ACTION_DISCONNECT },
        { "duplicate", ACTION_DUPLICATE },
        { "reordered", ACTION_REORDERED },
        { "invalid-nodeid", ACTION_INVALID_NODEID },
        { "invalid-attr", ACTION_INVALID_ATTR },
        { "invalid-parent", ACTION_INVALID_PARENT },
        { "invalid-write-count", ACTION_INVALID_WRITE_COUNT },
        { "invalid-write-offset", ACTION_INVALID_WRITE_OFFSET },
        { "invalid-read-count", ACTION_INVALID_READ_COUNT },
        { "invalid-readdir-count", ACTION_INVALID_READDIR_COUNT },
        { "invalid-readdir-record", ACTION_INVALID_READDIR_RECORD },
        { "invalid-statfs", ACTION_INVALID_STATFS },
        { "short-body", ACTION_SHORT_BODY },
        { "invalid-open-pad", ACTION_INVALID_OPEN_PAD },
        { "reply-disconnect", ACTION_REPLY_DISCONNECT },
        { "short-write", ACTION_SHORT_WRITE },
        { "reply-copy-fault", ACTION_REPLY_COPY_FAULT },
        { "random-bytes", ACTION_RANDOM_BYTES },
        { "hold", ACTION_HOLD }
    };

    for (size_t action_index = 0; action_index < sizeof (action_names) / sizeof (action_names[0]); ++action_index)
    {
        if (strcmp (name, action_names[action_index].name) == 0)
        {
            *action = action_names[action_index].action;
            return 0;
        }
    }
    return -1;
}

static int parse_scenario_rule (const char * line, const unsigned line_number)
{
    char opcode_name[32];
    char action_name[32];
    char extra[32];
    unsigned occurrence = 1;
    int error = 0;
    uint16_t opcode;
    enum action parsed_action;

    extra[0] = '\0';
    const int fields = sscanf (line, "%31s %31s %d %u %31s", opcode_name, action_name, &error, &occurrence, extra);
    if (fields < 2 || fields > 4 ||
        opcode_from_name (opcode_name, &opcode) != 0 ||
        action_from_name (action_name, &parsed_action) != 0 ||
        occurrence == 0 || rule_count == MAX_RULES ||
        (parsed_action == ACTION_ERROR &&
         (fields < 3 || error <= 0 || error > 127)))
    {
        fprintf (stderr, "scenario:%u: invalid rule\n", line_number);
        return -1;
    }
    if ((parsed_action == ACTION_DELAY ||
         parsed_action == ACTION_HOLD) &&
        (fields < 3 || error <= 0 || error > 30000))
    {
        fprintf (stderr, "scenario:%u: invalid delay or hold\n", line_number);
        return -1;
    }
    rules[rule_count].opcode = opcode;
    rules[rule_count].action = parsed_action;
    rules[rule_count].error = error;
    rules[rule_count].occurrence = occurrence;
    ++rule_count;
    return 0;
}

static int read_scenario_rules (FILE * scenario_file)
{
    char line[256];
    unsigned line_number = 0;

    while (fgets (line, sizeof (line), scenario_file) != NULL)
    {
        ++line_number;
        char * newline = strchr (line, '\n');
        if (newline == NULL && !feof (scenario_file))
        {
            fprintf (stderr, "scenario:%u: line too long\n", line_number);

            return -1;
        }
        if (newline != NULL)
            *newline = '\0';
        if (line_number == 1)
        {
            if (strcmp (line, "USFS-SCENARIO 1") != 0)
            {
                fprintf (stderr, "scenario: invalid version header\n");

                return -1;
            }
            continue;
        }
        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (parse_scenario_rule (line, line_number) != 0)
            return -1;
    }

    if (line_number == 0)
        return -1;

    return 0;
}

static int parse_scenario (const char * path)
{
    FILE * scenario_file = fopen (path, "r");
    if (scenario_file == NULL)
        return -1;

    const int rc = read_scenario_rules (scenario_file);
    fclose (scenario_file);
    if (rc != 0)
        errno = EINVAL;

    return rc;
}

static int barrier_wait (const char * ready_name, const char * release_name, const unsigned timeout_ms)
{
    char ready_path[PATH_MAX];
    char release_path[PATH_MAX];
    FILE * ready_file;
    unsigned elapsed_ms = 0;
    int ready_length;
    int release_length;

    if (barrier_directory == NULL || barrier_directory[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }
    ready_length = snprintf (ready_path, sizeof (ready_path), "%s/%s", barrier_directory, ready_name);
    release_length = snprintf (release_path, sizeof (release_path), "%s/%s", barrier_directory, release_name);
    if (ready_length < 0 || (size_t)ready_length >= sizeof (ready_path) ||
        release_length < 0 || (size_t)release_length >= sizeof (release_path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    ready_file = fopen (ready_path, "w");
    if (ready_file == NULL)
        return -1;
    fputs ("ready\n", ready_file);
    if (fclose (ready_file) != 0)
        return -1;

    while (elapsed_ms < timeout_ms)
    {
        if (stopping)
            return 1;
        if (access (release_path, F_OK) == 0)
            return 0;
        usleep (10000);
        elapsed_ms += 10;
    }
    errno = ETIMEDOUT;
    return -1;
}

static struct rule * select_rule (const struct usfs_in_hdr * request_header)
{
    struct rule * selected = NULL;

    if (force_sync_rules && request_header->opcode == USFS_OP_SYNCFS &&
        ((const struct usfs_syncfs_in *)(request_header + 1))->mode != USFS_SYNCFS_FORCE)
        return NULL;
    for (size_t rule_index = 0; rule_index < rule_count; ++rule_index)
    {
        if (rules[rule_index].opcode != request_header->opcode)
            continue;
        rules[rule_index].seen += 1;
        if (selected == NULL && rules[rule_index].seen == rules[rule_index].occurrence)
            selected = &rules[rule_index];
    }
    return selected;
}

static int terminated_field (const unsigned char * field, const size_t length, const size_t maximum)
{
    const unsigned char * end;

    if (length == 0)
        return false;

    if (length > maximum)
        return false;
    end = memchr (field, '\0', length);
    if (end == NULL)
        return false;

    return (size_t)(end - field) + 1 == length;
}

static int rename_request_valid (const unsigned char * body, const size_t body_length)
{
    const struct usfs_rename_in * request_body;
    size_t remaining;

    if (body_length <= sizeof (*request_body))
        return false;
    request_body = (const struct usfs_rename_in *)body;
    remaining = body_length - sizeof (*request_body);
    if (request_body->pad != 0)
        return false;

    if (request_body->newparent == 0)
        return false;

    if (request_body->oldnamelen == 0)
        return false;

    if (request_body->oldnamelen > remaining)
        return false;

    if (!terminated_field (body + sizeof (*request_body), request_body->oldnamelen, USFS_MAX_NAME))
        return false;

    return terminated_field (body + sizeof (*request_body) + request_body->oldnamelen, remaining - request_body->oldnamelen, USFS_MAX_NAME);
}

static int symlink_request_valid (const unsigned char * body, const size_t body_length)
{
    const struct usfs_symlink_in * request_body;
    size_t remaining;

    if (body_length <= sizeof (*request_body))
        return false;
    request_body = (const struct usfs_symlink_in *)body;
    remaining = body_length - sizeof (*request_body);
    if (request_body->pad != 0)
        return false;

    if (request_body->namelen == 0)
        return false;

    if (request_body->namelen > remaining)
        return false;

    if (!terminated_field (body + sizeof (*request_body), request_body->namelen, USFS_MAX_NAME))
        return false;

    return terminated_field (body + sizeof (*request_body) + request_body->namelen, remaining - request_body->namelen, USFS_MAX_LINK + 1u);
}

static int setattr_request_valid (const unsigned char * body, const size_t body_length)
{
    const struct usfs_setattr_in * request_body;
    const uint32_t allowed = USFS_SET_MODE | USFS_SET_UID | USFS_SET_GID |
                             USFS_SET_SIZE | USFS_SET_ATIME | USFS_SET_MTIME |
                             USFS_SET_CTIME | USFS_SET_TIMES_NOW;

    if (body_length != sizeof (*request_body))
        return false;
    request_body = (const struct usfs_setattr_in *)body;
    if ((request_body->valid & ~allowed) != 0)
        return false;

    if (request_body->atimensec >= 1000000000u)
        return false;

    if (request_body->mtimensec >= 1000000000u)
        return false;

    if (request_body->ctimensec >= 1000000000u)
        return false;

    return request_body->pad == 0;
}

static int read_request_valid (const unsigned char * body, const size_t body_length)
{
    const struct usfs_read_in * request_body;
    if (body_length != sizeof (*request_body))
        return false;
    request_body = (const struct usfs_read_in *)body;
    if (request_body->size > USFS_MAX_DATA)
        return false;

    return request_body->pad == 0;
}

static int readdir_request_valid (const unsigned char * body, const size_t body_length)
{
    const struct usfs_readdir_in * request_body;
    if (body_length != sizeof (*request_body))
        return false;
    request_body = (const struct usfs_readdir_in *)body;
    if (request_body->size > USFS_MAX_DATA)
        return false;

    return request_body->pad == 0;
}

static int write_request_valid (const unsigned char * body, const size_t body_length)
{
    const struct usfs_write_in * request_body;
    if (body_length < sizeof (*request_body))
        return false;
    request_body = (const struct usfs_write_in *)body;
    if (request_body->size > USFS_MAX_DATA)
        return false;

    if (request_body->size != body_length - sizeof (*request_body))
        return false;

    return usfs_write_flags_valid (request_body->flags);
}

static int flush_request_valid (const unsigned char * body, const size_t body_length)
{
    const struct usfs_flush_in * request_body;
    if (body_length != sizeof (*request_body))
        return false;
    request_body = (const struct usfs_flush_in *)body;
    return request_body->pad == 0;
}

static int request_body_valid (const uint16_t opcode, const unsigned char * body, const size_t body_length)
{
    switch (opcode)
    {
        case USFS_OP_FORGET:
            if (body_length != sizeof (struct usfs_forget_in))
                return false;

            return usfs_forget_in_valid ((const struct usfs_forget_in *)body, UINT64_MAX);
        case USFS_OP_LOOKUP:
        case USFS_OP_UNLINK:
        case USFS_OP_RMDIR:
            return terminated_field (body, body_length, USFS_MAX_NAME);
        case USFS_OP_GETATTR:
            return body_length == sizeof (struct usfs_getattr_in);
        case USFS_OP_OPEN:
            return body_length == sizeof (struct usfs_open_in);
        case USFS_OP_READ:
            return read_request_valid (body, body_length);
        case USFS_OP_RELEASE:
            return body_length == sizeof (struct usfs_release_in);
        case USFS_OP_READDIR:
            return readdir_request_valid (body, body_length);
        case USFS_OP_READLINK:
        case USFS_OP_STATFS:
            return body_length == 0;
        case USFS_OP_CREATE:
            if (body_length <= sizeof (struct usfs_create_in))
                return false;

            return terminated_field (body + sizeof (struct usfs_create_in), body_length - sizeof (struct usfs_create_in), USFS_MAX_NAME);
        case USFS_OP_CREATE_ATTR:
            if (body_length <= sizeof (struct usfs_create_attr_in))
                return false;

            if (!usfs_create_attr_in_valid ((const struct usfs_create_attr_in *)body))
                return false;

            return terminated_field (body + sizeof (struct usfs_create_attr_in), body_length - sizeof (struct usfs_create_attr_in), USFS_MAX_NAME);
        case USFS_OP_MKDIR:
            if (body_length <= sizeof (struct usfs_mkdir_in))
                return false;

            return terminated_field (body + sizeof (struct usfs_mkdir_in), body_length - sizeof (struct usfs_mkdir_in), USFS_MAX_NAME);
        case USFS_OP_RENAME:
            return rename_request_valid (body, body_length);
        case USFS_OP_SYMLINK:
            return symlink_request_valid (body, body_length);
        case USFS_OP_LINK:
            if (body_length <= sizeof (struct usfs_link_in))
                return false;

            if (((const struct usfs_link_in *)body)->newparent == 0)
                return false;

            return terminated_field (body + sizeof (struct usfs_link_in), body_length - sizeof (struct usfs_link_in), USFS_MAX_NAME);
        case USFS_OP_SETATTR:
            return setattr_request_valid (body, body_length);
        case USFS_OP_WRITE:
            return write_request_valid (body, body_length);
        case USFS_OP_FLUSH:
            return flush_request_valid (body, body_length);
        case USFS_OP_FSYNC:
            if (body_length != sizeof (struct usfs_fsync_in))
                return false;

            return usfs_fsync_in_valid ((const struct usfs_fsync_in *)body);
        case USFS_OP_SYNCFS:
            if (body_length != sizeof (struct usfs_syncfs_in))
                return false;

            return usfs_syncfs_in_valid ((const struct usfs_syncfs_in *)body);
        case USFS_OP_FID:
            return body_length == 0;
        case USFS_OP_VGET:
            if (body_length != sizeof (struct usfs_vget_in))
                return false;

            return ((const struct usfs_vget_in *)body)->token != 0;
        default:
            return false;
    }
}

static int request_valid (const unsigned char * request, const size_t request_length)
{
    const struct usfs_in_hdr * request_header = (const struct usfs_in_hdr *)request;

    if (request_length < sizeof (*request_header))
        return false;

    if (request_length > USFS_MSG_MAX)
        return false;

    if (request_header->len != request_length)
        return false;

    if (request_header->version != USFS_PROTOCOL_VERSION)
        return false;

    if (request_header->opcode < USFS_OP_LOOKUP)
        return false;

    if (request_header->opcode > USFS_OP_VGET)
        return false;

    if (request_header->opcode == USFS_OP_VGET && request_header->nodeid != USFS_ROOT_ID)
        return false;

    if (request_header->unique == 0)
        return false;

    if (request_header->nodeid == 0)
        return false;

    return request_body_valid (request_header->opcode, request + sizeof (*request_header), request_length - sizeof (*request_header));
}

static void fill_attr (struct usfs_attr * attributes, const uint64_t ino, const uint32_t mode, const uint64_t size)
{
    memset (attributes, 0, sizeof (*attributes));
    attributes->ino = ino;
    attributes->size = size;
    attributes->blocks = (size + 511u) / 512u;
    attributes->mode = mode;
    attributes->nlink = S_ISDIR (mode) ? 2 : 1;
    attributes->uid = getuid ();
    attributes->gid = getgid ();
    attributes->blksize = 4096;
}

static size_t append_dirent (unsigned char * destination, const uint64_t ino, const uint32_t type, const char * name)
{
    struct usfs_dirent * entry = (struct usfs_dirent *)destination;
    const size_t name_length = strlen (name);
    const size_t record_length = USFS_DIRENT_SIZE (name_length);

    memset (destination, 0, record_length);
    entry->ino = ino;
    entry->type = type;
    entry->namelen = (uint16_t)name_length;
    entry->reclen = (uint16_t)record_length;
    memcpy (destination + sizeof (*entry), name, name_length + 1);
    return record_length;
}

static size_t success_lookup_body (const struct usfs_in_hdr * request_header, unsigned char * body, int * reply_error)
{
    struct usfs_entry_out * entry = (struct usfs_entry_out *)body;
    const char * name = (const char *)(request_header + 1);
    struct test_node * test_node = find_node_name (request_header->nodeid, name);

    if (strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
    {
        uint64_t node_id = request_header->nodeid;

        test_node = find_node_id (node_id);
        if (strcmp (name, "..") == 0 && test_node != NULL)
            node_id = test_node->parent_id;
        test_node = find_node_id (node_id);
        if (node_id == USFS_ROOT_ID)
        {
            entry->nodeid = node_id;
            fill_attr (&entry->attr, node_id, S_IFDIR | 0755, 0);
            return sizeof (*entry);
        }
    }
    if (test_node == NULL)
    {
        *reply_error = ENOENT;
        return 0;
    }
    entry->nodeid = test_node->node_id;
    fill_attr (&entry->attr, test_node->node_id, test_node->mode, test_node->node_id == 2 ? scenario_file_size : 0);
    if (test_node->initial_attributes.ino != 0)
    {
        entry->attr = test_node->initial_attributes;
        entry->attr.mode = test_node->mode;
    }
    return sizeof (*entry);
}

static int read_root_attribute_fault (void)
{
    const char * fault_path = getenv ("USFS_SCENARIO_ROOT_FAULT_FILE");
    FILE * fault_file = fault_path == NULL ? NULL : fopen (fault_path, "r");
    if (fault_file == NULL)
        return 0;

    const int fault_kind = fgetc (fault_file);
    fclose (fault_file);
    return fault_kind;
}

static size_t success_getattr_body (const struct usfs_in_hdr * request_header, unsigned char * body, int * reply_error)
{
    struct usfs_attr_out * attributes;
    struct test_node * test_node;

    if (request_header->nodeid == USFS_ROOT_ID)
    {
        const int fault_kind = read_root_attribute_fault ();
        if (fault_kind == 'e')
        {
            *reply_error = EIO;
            return 0;
        }
        if (fault_kind == 'm')
        {
            memset (body, 0, sizeof (struct usfs_attr_out));
            return sizeof (struct usfs_attr_out);
        }
    }
    attributes = (struct usfs_attr_out *)body;
    test_node = find_node_id (request_header->nodeid);
    uint32_t mode = S_IFREG | 0644;
    if (request_header->nodeid == 1)
        mode = S_IFDIR | 0755;
    else if (test_node != NULL)
        mode = test_node->mode;
    fill_attr (&attributes->attr, request_header->nodeid, mode, request_header->nodeid == 2 ? scenario_file_size : 0);
    if (request_header->nodeid == USFS_ROOT_ID &&
        getenv ("USFS_SCENARIO_RESTRICTED_ROOT") != NULL)
        attributes->attr.mode = S_IFDIR | 0700;
    if (test_node != NULL && test_node->initial_attributes.ino != 0)
    {
        attributes->attr = test_node->initial_attributes;
        attributes->attr.mode = test_node->mode;
    }
    attributes->parent = 1;
    if (request_header->nodeid != 1 && test_node != NULL)
        attributes->parent = test_node->parent_id;
    return sizeof (*attributes);
}

static size_t success_setattr_body (const struct usfs_in_hdr * request_header, unsigned char * body)
{
    struct test_node * test_node = find_node_id (request_header->nodeid);
    struct usfs_attr_out * attributes;

    if (test_node != NULL)
    {
        const struct usfs_setattr_in * setattr_in =
            (const struct usfs_setattr_in *)(request_header + 1);

        if ((setattr_in->valid & USFS_SET_MODE) != 0)
            test_node->mode =
                (test_node->mode & S_IFMT) |
                (setattr_in->mode & ~S_IFMT);
    }
    attributes = (struct usfs_attr_out *)body;
    fill_attr (&attributes->attr, request_header->nodeid, test_node != NULL ? test_node->mode : S_IFREG | 0644, request_header->nodeid == 2 ? scenario_file_size : 0);
    attributes->parent = 1;
    return sizeof (*attributes);
}

static size_t success_read_body (const struct usfs_in_hdr * request_header, unsigned char * body)
{
    static const char content[] = "contract-data";
    const struct usfs_read_in * read_in =
        (const struct usfs_read_in *)(request_header + 1);
    size_t reply_length;

    if (full_reads)
    {
        for (reply_length = 0; reply_length < read_in->size; ++reply_length)
            body[reply_length] = (unsigned char)((read_in->offset + reply_length) & 0xffu);
        return read_in->size;
    }
    if (read_in->offset >= sizeof (content) - 1)
        return 0;
    reply_length = sizeof (content) - 1 - (size_t)read_in->offset;
    if (reply_length > read_in->size)
        reply_length = read_in->size;
    memcpy (body, content + read_in->offset, reply_length);
    return reply_length;
}

static size_t make_wide_directory_reply (const struct usfs_readdir_in * readdir_in, unsigned char * body, struct usfs_readdir_out * readdir_out)
{
    char generated_name[16];
    size_t reply_length;
    const uint64_t entry_index = readdir_in->cookie & USFS_DIRECTORY_CURSOR_INDEX_MASK;

    readdir_out->snapshot_id = readdir_in->cookie == 0 ? 1u : readdir_in->cookie >> USFS_DIRECTORY_CURSOR_INDEX_BITS;
    readdir_out->pad = 0;
    if (entry_index >= 3)
    {
        readdir_out->count = 0;
        return sizeof (*readdir_out);
    }
    snprintf (generated_name, sizeof (generated_name), "entry%llu", (unsigned long long)entry_index);
    readdir_out->count = 1;
    reply_length = sizeof (*readdir_out);
    reply_length += append_dirent (body + reply_length, 20 + entry_index, 8, generated_name);
    return reply_length;
}

static size_t success_readdir_body (const struct usfs_in_hdr * request_header, unsigned char * body)
{
    struct usfs_readdir_out * readdir_out =
        (struct usfs_readdir_out *)body;
    const struct usfs_readdir_in * readdir_in =
        (const struct usfs_readdir_in *)(request_header + 1);
    size_t reply_length;

    if (wide_readdir)
        return make_wide_directory_reply (readdir_in, body, readdir_out);

    readdir_out->snapshot_id = readdir_in->cookie == 0 ? 1u : readdir_in->cookie >> USFS_DIRECTORY_CURSOR_INDEX_BITS;

    if (readdir_in->cookie != 0)
    {
        readdir_out->count = 0;
        readdir_out->pad = 0;
        return sizeof (*readdir_out);
    }
    readdir_out->count = 2;
    readdir_out->pad = 0;
    reply_length = sizeof (*readdir_out);
    reply_length += append_dirent (body + reply_length, 2, 8, "file");
    reply_length += append_dirent (body + reply_length, 3, 4, "dir");
    return reply_length;
}

static void apply_created_node_attributes (struct test_node * test_node, const struct usfs_in_hdr * request_header, const struct usfs_setattr_in * attributes, const uint64_t node_id)
{
    fill_attr (&test_node->initial_attributes, node_id, test_node->mode, attributes->valid & USFS_SET_SIZE ? attributes->size : 0);
    test_node->initial_attributes.uid =
        attributes->valid & USFS_SET_UID ? attributes->uid : request_header->uid;
    test_node->initial_attributes.gid =
        attributes->valid & USFS_SET_GID ? attributes->gid : request_header->gid;
    if (attributes->valid & USFS_SET_ATIME)
    {
        test_node->initial_attributes.atime = attributes->atime;
        test_node->initial_attributes.atimensec = attributes->atimensec;
    }
    if (attributes->valid & USFS_SET_MTIME)
    {
        test_node->initial_attributes.mtime = attributes->mtime;
        test_node->initial_attributes.mtimensec = attributes->mtimensec;
    }
    if (attributes->valid & USFS_SET_CTIME)
    {
        test_node->initial_attributes.ctime = attributes->ctime;
        test_node->initial_attributes.ctimensec = attributes->ctimensec;
    }
}

static size_t success_create_attr_body (const struct usfs_in_hdr * request_header, unsigned char * body, int * reply_error)
{
    const struct usfs_create_attr_in * request_body =
        (const struct usfs_create_attr_in *)(request_header + 1);
    const struct usfs_setattr_in * attributes = &request_body->attr;
    const uint64_t node_id = request_header->unique + 100;
    const char * name = (const char *)(request_body + 1);
    struct usfs_create_out * create_out;
    struct test_node * test_node;

    if (find_node_name (request_header->nodeid, name) != NULL)
    {
        *reply_error = EEXIST;
        return 0;
    }
    /* The fixed node table reserves all publication storage in add_node. */
    if (add_node (name, node_id, request_header->nodeid, S_IFREG | attributes->mode) != 0)
    {
        *reply_error = ENOSPC;
        return 0;
    }
    test_node = find_node_id (node_id);
    apply_created_node_attributes (test_node, request_header, attributes, node_id);
    create_out = (struct usfs_create_out *)body;
    memset (create_out, 0, sizeof (*create_out));
    create_out->nodeid =
        request_body->activation == USFS_CREATE_DEFAULT ? 0 : node_id;
    create_out->fh = request_body->activation == USFS_CREATE_OPEN ? node_id : 0;
    create_out->attr = test_node->initial_attributes;
    return sizeof (*create_out);
}

static size_t success_create_body (const struct usfs_in_hdr * request_header, unsigned char * body, int * reply_error)
{
    const char * name =
        (const char *)(request_header + 1) + sizeof (struct usfs_create_in);
    struct usfs_create_out * create_out =
        (struct usfs_create_out *)body;

    memset (create_out, 0, sizeof (*create_out));
    create_out->nodeid = request_header->unique + 100;
    create_out->fh = create_out->nodeid;
    fill_attr (&create_out->attr, create_out->nodeid, S_IFREG | 0644, 0);
    if (add_node (name, create_out->nodeid, request_header->nodeid, S_IFREG | 0644) != 0)
        *reply_error = ENOSPC;
    return sizeof (*create_out);
}

static size_t success_entry_body (const struct usfs_in_hdr * request_header, unsigned char * body, int * reply_error)
{
    struct usfs_entry_out * entry = (struct usfs_entry_out *)body;
    const struct usfs_link_in * link_in = NULL;
    const char * name;

    entry->nodeid = request_header->opcode == USFS_OP_LINK ? request_header->nodeid : request_header->unique + 100;
    uint32_t mode = S_IFREG | 0644;
    if (request_header->opcode == USFS_OP_MKDIR)
        mode = S_IFDIR | 0755;
    else if (request_header->opcode == USFS_OP_SYMLINK)
        mode = S_IFLNK | 0777;
    fill_attr (&entry->attr, entry->nodeid, mode, 0);
    if (request_header->opcode == USFS_OP_MKDIR)
    {
        name = (const char *)((const struct usfs_mkdir_in *)(request_header + 1) + 1);
    }
    else if (request_header->opcode == USFS_OP_SYMLINK)
    {
        const struct usfs_symlink_in * symlink_in =
            (const struct usfs_symlink_in *)(request_header + 1);

        name = (const char *)(symlink_in + 1);
    }
    else
    {
        link_in = (const struct usfs_link_in *)(request_header + 1);
        name = (const char *)(link_in + 1);
    }
    if (add_node (name, entry->nodeid, request_header->opcode == USFS_OP_LINK ? link_in->newparent : request_header->nodeid, entry->attr.mode) != 0)
        *reply_error = ENOSPC;
    return 0;
}

static size_t success_open_body (const struct usfs_in_hdr * request_header, unsigned char * body)
{
    struct usfs_open_out * open_out;

    open_out = (struct usfs_open_out *)body;
    memset (open_out, 0, sizeof (*open_out));
    open_out->fh = request_header->unique;
    return sizeof (*open_out);
}

static size_t success_rename_body (const struct usfs_in_hdr * request_header)
{
    const struct usfs_rename_in * rename_in;
    const char * name;
    const char * second_name;
    struct test_node * test_node;

    rename_in = (const struct usfs_rename_in *)(request_header + 1);
    name = (const char *)(rename_in + 1);
    second_name = name + rename_in->oldnamelen;
    test_node = find_node_name (request_header->nodeid, name);
    if (test_node != NULL)
    {
        strncpy (test_node->name, second_name, USFS_MAX_NAME - 1);
        test_node->name[USFS_MAX_NAME - 1] = '\0';
        test_node->parent_id = rename_in->newparent;
    }
    return 0;
}

static size_t success_statfs_body (unsigned char * body)
{
    struct usfs_statfs_out * statfs_out;

    statfs_out = (struct usfs_statfs_out *)body;
    memset (statfs_out, 0, sizeof (*statfs_out));
    statfs_out->blocks = 1024;
    statfs_out->bfree = statfs_out->bavail = 512;
    statfs_out->files = 128;
    statfs_out->ffree = 120;
    statfs_out->bsize = 4096;
    statfs_out->namemax = USFS_MAX_NAME - 1;
    if (getenv ("USFS_SCENARIO_STATFS_CAPACITY") != NULL)
    {
        statfs_out->bfree = statfs_out->bavail = statfs_out->ffree = 0;
        if (strcmp (getenv ("USFS_SCENARIO_STATFS_CAPACITY"), "zero") == 0)
            statfs_out->blocks = statfs_out->files = 0;
    }
    return sizeof (*statfs_out);
}

static size_t success_write_body (const struct usfs_in_hdr * request_header, unsigned char * body)
{
    struct usfs_write_out * write_out;

    write_out = (struct usfs_write_out *)body;
    write_out->written =
        ((const struct usfs_write_in *)(request_header + 1))->size;
    write_out->pad = 0;
    write_out->offset =
        ((const struct usfs_write_in *)(request_header + 1))->offset;
    return sizeof (*write_out);
}

static size_t success_body (const struct usfs_in_hdr * request_header, unsigned char * body, const size_t capacity, int * reply_error)
{
    (void)capacity;
    switch (request_header->opcode)
    {
        case USFS_OP_LOOKUP:
            return success_lookup_body (request_header, body, reply_error);
        case USFS_OP_GETATTR:
            return success_getattr_body (request_header, body, reply_error);
        case USFS_OP_SETATTR:
            return success_setattr_body (request_header, body);
        case USFS_OP_OPEN:
            return success_open_body (request_header, body);
        case USFS_OP_READ:
            return success_read_body (request_header, body);
        case USFS_OP_UNLINK:
        case USFS_OP_RMDIR:
            remove_node (request_header->nodeid, (const char *)(request_header + 1));
            return 0;
        case USFS_OP_RENAME:
            return success_rename_body (request_header);
        case USFS_OP_READDIR:
            return success_readdir_body (request_header, body);
        case USFS_OP_READLINK:
            /* Binary reply length is explicit; no trailing NUL is on the wire. */
            /* NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
            memcpy (body, "file", 4);
            return 4;
        case USFS_OP_STATFS:
            return success_statfs_body (body);
        case USFS_OP_CREATE_ATTR:
            return success_create_attr_body (request_header, body, reply_error);
        case USFS_OP_CREATE:
            return success_create_body (request_header, body, reply_error);
        case USFS_OP_MKDIR:
        case USFS_OP_SYMLINK:
        case USFS_OP_LINK:
            return success_entry_body (request_header, body, reply_error);
        case USFS_OP_WRITE:
            return success_write_body (request_header, body);
        case USFS_OP_FLUSH:
        case USFS_OP_FSYNC:
        case USFS_OP_SYNCFS:
            return 0;
        case USFS_OP_RELEASE:
            *reply_error = handle_ownership (((const struct usfs_release_in *)(request_header + 1))->fh, 0);
            return 0;
        case USFS_OP_FORGET:
            *reply_error = lookup_ownership (request_header->nodeid, ((const struct usfs_forget_in *)(request_header + 1))->count, 0);
            return 0;
        case USFS_OP_FID:
        case USFS_OP_VGET:
            *reply_error = EOPNOTSUPP;
            return 0;
        default:
            return 0;
    }
}

static void corrupt_node_identifier (const struct usfs_in_hdr * request_header, unsigned char * body, const size_t body_length)
{
    if (request_header->opcode == USFS_OP_LOOKUP &&
        body_length >= sizeof (struct usfs_entry_out))
        ((struct usfs_entry_out *)body)->nodeid = 0;
    else if ((request_header->opcode == USFS_OP_CREATE || request_header->opcode == USFS_OP_CREATE_ATTR) && body_length >= sizeof (struct usfs_create_out))
        ((struct usfs_create_out *)body)->nodeid = 0;
}

static void corrupt_reply_attributes (const struct usfs_in_hdr * request_header, unsigned char * body, const size_t body_length)
{
    if (request_header->opcode == USFS_OP_LOOKUP &&
        body_length >= sizeof (struct usfs_entry_out))
        ((struct usfs_entry_out *)body)->attr.atimensec =
            1000000000u;
    else if (request_header->opcode == USFS_OP_GETATTR && body_length >= sizeof (struct usfs_attr_out))
        ((struct usfs_attr_out *)body)->attr.atimensec =
            1000000000u;
    else if ((request_header->opcode == USFS_OP_CREATE || request_header->opcode == USFS_OP_CREATE_ATTR) && body_length >= sizeof (struct usfs_create_out))
        ((struct usfs_create_out *)body)->attr.atimensec =
            1000000000u;
}

static void corrupt_operation_body (const enum action action, const struct usfs_in_hdr * request_header, unsigned char * body, const size_t body_length)
{
    switch (action)
    {
        case ACTION_INVALID_NODEID:
            corrupt_node_identifier (request_header, body, body_length);
            break;
        case ACTION_INVALID_ATTR:
            corrupt_reply_attributes (request_header, body, body_length);
            break;
        case ACTION_INVALID_PARENT:
            if (request_header->opcode == USFS_OP_GETATTR &&
                body_length >= sizeof (struct usfs_attr_out))
                ((struct usfs_attr_out *)body)->parent = 0;
            break;
        case ACTION_INVALID_WRITE_COUNT:
            if (request_header->opcode == USFS_OP_WRITE &&
                body_length >= sizeof (struct usfs_write_out))
                ((struct usfs_write_out *)body)->written =
                    ((const struct usfs_write_in *)(request_header + 1))->size + 1u;
            break;
        case ACTION_INVALID_WRITE_OFFSET:
            if (request_header->opcode == USFS_OP_WRITE &&
                body_length >= sizeof (struct usfs_write_out))
            {
                ((struct usfs_write_out *)body)->written = 1;
                ((struct usfs_write_out *)body)->offset = UINT64_MAX;
            }
            break;
        case ACTION_INVALID_READDIR_COUNT:
            if (request_header->opcode == USFS_OP_READDIR &&
                body_length >= sizeof (struct usfs_readdir_out))
                ((struct usfs_readdir_out *)body)->count += 1u;
            break;
        case ACTION_INVALID_READDIR_RECORD:
            if (request_header->opcode == USFS_OP_READDIR &&
                body_length >= sizeof (struct usfs_readdir_out) +
                                   sizeof (struct usfs_dirent))
                ((struct usfs_dirent *)(body + sizeof (struct usfs_readdir_out)))->reclen =
                    (uint16_t)sizeof (struct usfs_dirent);
            break;
        case ACTION_INVALID_STATFS:
            if (request_header->opcode == USFS_OP_STATFS &&
                body_length >= sizeof (struct usfs_statfs_out))
                ((struct usfs_statfs_out *)body)->bfree =
                    ((struct usfs_statfs_out *)body)->blocks + 1u;
            break;
        case ACTION_INVALID_OPEN_PAD:
            if (request_header->opcode == USFS_OP_OPEN &&
                body_length >= sizeof (struct usfs_open_out))
                ((struct usfs_open_out *)body)->pad = 1;
            break;
        case ACTION_SHORT_WRITE:
            if (request_header->opcode == USFS_OP_WRITE &&
                body_length >= sizeof (struct usfs_write_out))
                ((struct usfs_write_out *)body)->written =
                    ((const struct usfs_write_in *)(request_header + 1))->size / 2u;
            break;
        default:
            break;
    }
}

static void log_read_request (const struct usfs_in_hdr * request_header)
{
    const struct usfs_read_in * request_body =
        (const struct usfs_read_in *)(request_header + 1);
    fprintf (request_log, "HANDLE %u %llu\n", (unsigned)request_header->opcode, (unsigned long long)request_body->fh);
    fprintf (request_log, "DETAIL %u %llu %u\n", (unsigned)request_header->opcode, (unsigned long long)request_body->offset, (unsigned)request_body->size);
}

static void log_write_request (const struct usfs_in_hdr * request_header)
{
    const struct usfs_write_in * request_body =
        (const struct usfs_write_in *)(request_header + 1);
    fprintf (request_log, "HANDLE %u %llu\n", (unsigned)request_header->opcode, (unsigned long long)request_body->fh);
    fprintf (request_log, "DETAIL %u %llu %u\n", (unsigned)request_header->opcode, (unsigned long long)request_body->offset, (unsigned)request_body->size);
}

static void log_readdir_request (const struct usfs_in_hdr * request_header)
{
    const struct usfs_readdir_in * request_body =
        (const struct usfs_readdir_in *)(request_header + 1);
    fprintf (request_log, "DETAIL %u %llu %u\n", (unsigned)request_header->opcode, (unsigned long long)request_body->cookie, (unsigned)request_body->size);
    fprintf (request_log, "HANDLE %u %llu\n", (unsigned)request_header->opcode, (unsigned long long)request_body->fh);
}

static void log_release_request (const struct usfs_in_hdr * request_header)
{
    const struct usfs_release_in * request_body =
        (const struct usfs_release_in *)(request_header + 1);
    fprintf (request_log, "HANDLE %u %llu\n", (unsigned)request_header->opcode, (unsigned long long)request_body->fh);
}

static void log_flush_request (const struct usfs_in_hdr * request_header)
{
    const struct usfs_flush_in * request_body =
        (const struct usfs_flush_in *)(request_header + 1);
    fprintf (request_log, "DETAIL %u %llu %u\n", (unsigned)request_header->opcode, (unsigned long long)request_body->fh, (unsigned)request_body->flags);
}

static void log_fsync_request (const struct usfs_in_hdr * request_header)
{
    const struct usfs_fsync_in * request_body =
        (const struct usfs_fsync_in *)(request_header + 1);
    fprintf (request_log, "DETAIL %u %llu %llu %llu %u\n", (unsigned)request_header->opcode, (unsigned long long)request_body->fh, (unsigned long long)request_body->offset, (unsigned long long)request_body->length, (unsigned)request_body->flags);
}

static void log_syncfs_request (const struct usfs_in_hdr * request_header)
{
    const struct usfs_syncfs_in * request_body =
        (const struct usfs_syncfs_in *)(request_header + 1);
    fprintf (request_log, "DETAIL %u %u\n", (unsigned)request_header->opcode, (unsigned)request_body->mode);
}

static void log_create_attr_request (const struct usfs_in_hdr * request_header)
{
    const struct usfs_create_attr_in * request_body =
        (const struct usfs_create_attr_in *)(request_header + 1);
    fprintf (request_log, "CREATE_ATTR flags=%u activation=%u valid=%u\n", (unsigned)request_body->flags, (unsigned)request_body->activation, (unsigned)request_body->attr.valid);
}

static void log_request (const struct usfs_in_hdr * request_header)
{
    fprintf (request_log, "%u %llu %llu %u %u %u %u\n", (unsigned)request_header->opcode, (unsigned long long)request_header->unique, (unsigned long long)request_header->nodeid, (unsigned)request_header->len, (unsigned)request_header->uid, (unsigned)request_header->gid, (unsigned)request_header->pid);
    switch (request_header->opcode)
    {
        case USFS_OP_READ:
            log_read_request (request_header);
            break;
        case USFS_OP_WRITE:
            log_write_request (request_header);
            break;
        case USFS_OP_READDIR:
            log_readdir_request (request_header);
            break;
        case USFS_OP_RELEASE:
            log_release_request (request_header);
            break;
        case USFS_OP_FLUSH:
            log_flush_request (request_header);
            break;
        case USFS_OP_FSYNC:
            log_fsync_request (request_header);
            break;
        case USFS_OP_SYNCFS:
            log_syncfs_request (request_header);
            break;
        case USFS_OP_CREATE_ATTR:
            log_create_attr_request (request_header);
            break;
        default:
            break;
    }
    fflush (request_log);
}

static int acquire_reply_ownership (const struct usfs_in_hdr * request_header, const struct usfs_reply_header * reply_header, const size_t body_length)
{
    uint64_t acquired_node_id = 0;

    if (reply_header->error != 0)
        return 0;

    if (request_header->opcode == USFS_OP_LOOKUP &&
        body_length == sizeof (struct usfs_entry_out))
        acquired_node_id = ((const struct usfs_entry_out *)(reply_header + 1))->nodeid;
    if (request_header->opcode == USFS_OP_OPEN &&
        body_length == sizeof (struct usfs_open_out) &&
        handle_ownership (((const struct usfs_open_out *)(reply_header + 1))->fh, 1) != 0)
        return 1;

    if ((request_header->opcode == USFS_OP_CREATE ||
         request_header->opcode == USFS_OP_CREATE_ATTR) &&
        body_length == sizeof (struct usfs_create_out))
    {
        const struct usfs_create_out * created =
            (const struct usfs_create_out *)(reply_header + 1);
        const uint32_t activation = request_header->opcode == USFS_OP_CREATE ? USFS_CREATE_OPEN : ((const struct usfs_create_attr_in *)(request_header + 1))->activation;

        if (activation != USFS_CREATE_DEFAULT)
            acquired_node_id = created->nodeid;
        if (activation == USFS_CREATE_OPEN &&
            handle_ownership (created->fh, 1) != 0)
            return 1;
    }
    if (acquired_node_id != 0 && lookup_ownership (acquired_node_id, 1, 1) != 0)
        return 1;

    return 0;
}

static void corrupt_reply (const struct rule * rule, const struct usfs_in_hdr * request_header, unsigned char * reply, struct usfs_reply_header * reply_header, const size_t body_length, size_t * write_length)
{
    corrupt_operation_body (rule->action, request_header, reply + sizeof (*reply_header), body_length);
    switch (rule->action)
    {
        case ACTION_WRONG_VERSION:
            reply_header->version += 1;
            break;
        case ACTION_WRONG_OPCODE:
            reply_header->opcode = USFS_OP_WRITE;
            break;
        case ACTION_WRONG_UNIQUE:
            reply_header->id += 1;
            break;
        case ACTION_TRUNCATED:
            reply_header->len = sizeof (*reply_header) - 1;
            *write_length = sizeof (*reply_header) - 1;
            break;
        case ACTION_OVERSIZED:
            reply_header->len = USFS_MSG_MAX + 1;
            *write_length = sizeof (*reply_header);
            break;
        case ACTION_WRONG_BODY:
            reply_header->len = sizeof (*reply_header) + 1;
            *write_length = reply_header->len;
            break;
        case ACTION_SHORT_BODY:
            if (body_length > 0)
            {
                reply_header->len -= 1;
                *write_length -= 1;
            }
            break;
        case ACTION_RANDOM_BYTES:
            *write_length = sizeof (*reply_header);
            for (size_t byte_index = 0; byte_index < *write_length; ++byte_index)
                reply[byte_index] = (unsigned char)((request_header->unique >> ((byte_index % 8u) * 8u)) ^ (uint64_t)(byte_index * 0x5du));
            break;
        default:
            break;
    }
}

static int apply_reply_control_action (const struct rule * rule, const struct usfs_in_hdr * request_header)
{
    if (rule != NULL && rule->action == ACTION_DISCONNECT)
        return 1;

    if (rule != NULL && rule->action == ACTION_REORDERED)
        return 2;

    if (rule != NULL && rule->action == ACTION_HOLD)
    {
        if (request_header->opcode == USFS_OP_SETATTR &&
            getenv ("USFS_SCENARIO_COMMIT_SETATTR_BEFORE_HOLD") != NULL)
        {
            const struct usfs_setattr_in * change = (const struct usfs_setattr_in *)(request_header + 1);
            if (request_header->nodeid == 2 && (change->valid & USFS_SET_SIZE) != 0)
            {
                scenario_file_size = change->size;
                fprintf (request_log, "COMMITTED_SIZE %llu\n", (unsigned long long)scenario_file_size);
                fflush (request_log);
            }
        }
        const int barrier_rc = barrier_wait ("after-read.ready", "after-read.release", (unsigned)rule->error);
        if (barrier_rc != 0)
            return barrier_rc;
    }
    if (rule != NULL && rule->action == ACTION_DELAY)
        usleep ((useconds_t)rule->error * 1000u);
    return 0;
}

static int write_scenario_reply (const int device_descriptor, const struct rule * rule, const struct usfs_in_hdr * request_header, unsigned char * reply, const size_t write_length)
{
    const struct usfs_reply_header * reply_header = (const struct usfs_reply_header *)reply;
    ssize_t written;
    if (rule != NULL && rule->action == ACTION_REPLY_COPY_FAULT)
    {
        /* AIX's low page is readable; use an address outside user space. */
        volatile uintptr_t invalid_address = UINTPTR_MAX;
        struct iovec vectors[2] = {
            { (void *)reply, sizeof (*reply_header) },
            { (void *)invalid_address, write_length - sizeof (*reply_header) }
        };
        written = writev (device_descriptor, vectors, 2);
    }
    else
        written = write (device_descriptor, reply, write_length);
    if (written != (ssize_t)write_length)
    {
        fprintf (request_log, "REPLY_REJECTED errno=%d\n", errno);
        return -1;
    }
    fprintf (request_log, "REPLIED %llu\n", (unsigned long long)request_header->unique);
    fflush (request_log);
    if (rule != NULL && rule->action == ACTION_REPLY_DISCONNECT)
        return 1;

    if (rule != NULL && rule->action == ACTION_DUPLICATE)
    {
        if (write (device_descriptor, reply, write_length) < 0)
            fprintf (request_log, "DUPLICATE_REJECTED %llu\n", (unsigned long long)request_header->unique);
        else
            fprintf (request_log, "DUPLICATE_ACCEPTED %llu\n", (unsigned long long)request_header->unique);
        fflush (request_log);
    }
    return 0;
}

static size_t prepare_reply_body (const struct rule * rule, const struct usfs_in_hdr * request_header, unsigned char * reply, const size_t reply_capacity, struct usfs_reply_header * reply_header)
{
    size_t body_length;

    if (rule != NULL && rule->action == ACTION_ERROR)
    {
        reply_header->error = rule->error;
        body_length = 0;
    }
    else
    {
        body_length = success_body (request_header, reply + sizeof (*reply_header), reply_capacity - sizeof (*reply_header), &reply_header->error);
        if (reply_header->error != 0)
            body_length = 0;
    }
    if (rule != NULL && rule->action == ACTION_INVALID_READ_COUNT &&
        request_header->opcode == USFS_OP_READ &&
        body_length < reply_capacity - sizeof (*reply_header))
    {
        reply[sizeof (*reply_header) + body_length] = 0xee;
        body_length += 1;
    }
    return body_length;
}

static int reply_request (const int device_descriptor, unsigned char * request, const ssize_t request_length)
{
    const struct usfs_in_hdr * request_header = (const struct usfs_in_hdr *)request;
    unsigned char reply[USFS_MSG_MAX + 64];
    struct usfs_reply_header * reply_header = (struct usfs_reply_header *)reply;
    size_t write_length;

    if (request_length < 0 ||
        !request_valid (request, (size_t)request_length))
    {
        fprintf (request_log, "INVALID %ld\n", (long)request_length);
        fflush (request_log);
        return -1;
    }
    log_request (request_header);

    memset (reply, 0, sizeof (reply));
    reply_header->version = USFS_PROTOCOL_VERSION;
    reply_header->opcode = request_header->opcode;
    reply_header->id = request_header->unique;
    struct rule * rule = select_rule (request_header);
    if (rule != NULL)
    {
        fprintf (request_log, "ACTION %u %d %d\n", (unsigned)request_header->opcode, (int)rule->action, rule->error);
        fflush (request_log);
    }
    const int action_rc = apply_reply_control_action (rule, request_header);
    if (action_rc != 0)
        return action_rc;

    const size_t body_length = prepare_reply_body (rule, request_header, reply, sizeof (reply), reply_header);
    reply_header->len = (uint32_t)(sizeof (*reply_header) + body_length);
    write_length = reply_header->len;
    /* Allocate wire ownership before corrupting the successful reply. This
     * models a backend that committed its OPEN/LOOKUP/CREATE already. */
    if (acquire_reply_ownership (request_header, reply_header, body_length) != 0)
        return 1;

    if (rule != NULL)
        corrupt_reply (rule, request_header, reply, reply_header, body_length, &write_length);
    return write_scenario_reply (device_descriptor, rule, request_header, reply, write_length);
}

static int configure_scenario_environment (void)
{
    char * file_size_end;

    barrier_directory = getenv ("USFS_SCENARIO_BARRIER_DIR");
    force_sync_rules = getenv ("USFS_SCENARIO_FORCE_SYNC_RULES") != NULL;
    full_reads = getenv ("USFS_SCENARIO_FULL_READS") != NULL;
    wide_readdir = getenv ("USFS_SCENARIO_WIDE_READDIR") != NULL;

    const char * file_size_text = getenv ("USFS_SCENARIO_FILE_SIZE");
    if (file_size_text == NULL)
        return 0;

    errno = 0;
    const unsigned long long parsed_file_size = strtoull (file_size_text, &file_size_end, 0);
    if (errno != 0 || file_size_text[0] == '\0' ||
        file_size_end[0] != '\0' ||
        parsed_file_size > (unsigned long long)INT64_MAX)
    {
        fprintf (stderr, "invalid USFS_SCENARIO_FILE_SIZE\n");
        return 2;
    }
    scenario_file_size = (uint64_t)parsed_file_size;
    return 0;
}

static void initialize_scenario_nodes (void)
{
    add_node ("file", 2, 1, S_IFREG | 0644);
    add_node ("dir", 3, 1, S_IFDIR | 0755);
    add_node ("link", 4, 1, S_IFLNK | 0777);
    add_node ("child", 5, 3, S_IFDIR | 0755);
    add_node ("leaf", 6, 5, S_IFREG | 0644);
}

static int run_authority_probe (const char * mountpoint)
{
    struct usfs_dev_info device_info;
    const int device_descriptor = open (DEVICE, O_RDWR);

    if (device_descriptor < 0 || ioctl (device_descriptor, USFS_IOC_CONNECTION_REQUEST, &device_info) != 0)
    {
        if (device_descriptor >= 0)
            close (device_descriptor);
        return 1;
    }
    puts ("CHANNEL_ADMITTED");
    errno = 0;
    const int mounted = mount_channel (device_descriptor, &device_info, mountpoint);
    const int error = errno;
    close (device_descriptor);
    if (mounted != 0 && (error == EPERM || error == EACCES))
    {
        puts ("MOUNT_AUTHORITY_DENIED");
        return 0;
    }
    fprintf (stderr, "unexpected mount result=%d errno=%d\n", mounted, error);
    return 1;
}

static int open_scenario_channel (const char * mountpoint, int * device_descriptor_out)
{
    struct usfs_dev_info device_info;
    const char * race_mount = getenv ("USFS_SCENARIO_RACE_MOUNT");
    const int device_descriptor = open (DEVICE, O_RDWR);

    *device_descriptor_out = device_descriptor;
    if (device_descriptor < 0 || ioctl (device_descriptor, USFS_IOC_CONNECTION_REQUEST, &device_info) != 0 ||
        device_info.protocol_version != USFS_PROTOCOL_VERSION ||
        (race_mount != NULL ? mount_channel_pair (device_descriptor, &device_info, mountpoint, race_mount) : mount_channel (device_descriptor, &device_info, mountpoint)) != 0)
    {
        perror ("scenario daemon setup");
        return 1;
    }

    const char * second_mount = getenv ("USFS_SCENARIO_SECOND_MOUNT");
    if (second_mount != NULL)
    {
        errno = 0;
        if (mount_channel (device_descriptor, &device_info, second_mount) == 0 || errno != EBUSY)
            return 1;
        puts ("SECOND_MOUNT_BUSY");
    }
    return 0;
}

static int wait_before_read (void)
{
    if (getenv ("USFS_SCENARIO_PAUSE_BEFORE_READ") == NULL)
        return 0;

    return barrier_wait ("before-read.ready", "before-read.release", 30000);
}

static int serve_requests (const int device_descriptor)
{
    unsigned char request[USFS_MSG_MAX];
    unsigned char deferred[USFS_MSG_MAX];
    ssize_t deferred_length = 0;
    int rc = 0;

    while (!stopping)
    {
        const ssize_t bytes_read = read (device_descriptor, request, sizeof (request));
        int reply_rc;

        if (bytes_read < 0)
        {
            if (errno == EINTR && stopping)
                break;
            rc = 1;
            break;
        }
        if (bytes_read == 0)
        {
            fprintf (request_log, "ORDERLY_EOF\n");
            break;
        }
        reply_rc = reply_request (device_descriptor, request, bytes_read);
        if (reply_rc == 2)
        {
            if (deferred_length != 0)
            {
                rc = 1;
                break;
            }
            memcpy (deferred, request, (size_t)bytes_read);
            deferred_length = bytes_read;
            continue;
        }
        if (reply_rc != 0)
        {
            if (reply_rc < 0)
                rc = 1;
            break;
        }
        if (deferred_length != 0)
        {
            reply_rc = reply_request (device_descriptor, deferred, deferred_length);
            deferred_length = 0;
            if (reply_rc != 0)
            {
                rc = 1;
                break;
            }
        }
    }
    return rc;
}

static int run_scenario_session (const char * mountpoint)
{
    int device_descriptor = INVALID_FILE_DESCRIPTOR;
    int rc;

    if (open_scenario_channel (mountpoint, &device_descriptor) != 0)
    {
        if (device_descriptor >= 0)
            close (device_descriptor);
        return 1;
    }
    signal (SIGTERM, on_signal);
    signal (SIGINT, on_signal);
    printf ("READY %s\n", mountpoint);
    fflush (stdout);
    const int barrier_rc = wait_before_read ();
    if (barrier_rc != 0)
    {
        rc = barrier_rc < 0 ? 1 : 0;
        close (device_descriptor);
        return rc;
    }
    rc = serve_requests (device_descriptor);
    close (device_descriptor);
    close_ownership ();
    return rc;
}

int main (const int argc, char ** argv)
{
    int rc;

    if (argc != 4)
    {
        fprintf (stderr, "usage: %s MOUNTPOINT SCENARIO REQUEST_LOG\n", argv[0]);
        return 2;
    }
    if (parse_scenario (argv[2]) != 0)
    {
        perror ("scenario");
        return 2;
    }
    rc = configure_scenario_environment ();
    if (rc != 0)
        return rc;
    initialize_scenario_nodes ();
    request_log = fopen (argv[3], "w");
    if (request_log == NULL)
    {
        perror ("request log");
        return 2;
    }
    if (getenv ("USFS_SCENARIO_AUTHORITY_PROBE") != NULL)
    {
        rc = run_authority_probe (argv[1]);
        fclose (request_log);
        return rc;
    }
    rc = run_scenario_session (argv[1]);
    fclose (request_log);
    return rc;
}

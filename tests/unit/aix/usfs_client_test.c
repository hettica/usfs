/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */
#include "tap.h"
#include <usfs/usfs.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/vmount.h>
#include <sys/wait.h>
#include <poll.h>
#include "usfs_proto.h"
#include "usfs_validate.h"

static int allocation_tracking, fail_mount_calloc, fail_mount_strdup, fail_mount_realloc;
static void * mount_allocations[4];
static unsigned live_allocations;
static unsigned directory_fail_growth, directory_growths;
static unsigned fail_client_calloc;
static int fail_context_key_create;
static unsigned fail_signal_install_call, signal_install_calls;
static int signal_test_sigaction (const int signal_number, const struct sigaction * action, struct sigaction * previous)
{
    if (action != NULL && fail_signal_install_call != 0 && ++signal_install_calls == fail_signal_install_call)
    {
        fail_signal_install_call = 0;
        errno = EACCES;
        return -1;
    }

    return sigaction (signal_number, action, previous);
}

static int context_test_key_create (pthread_key_t * key, void (*destroy) (void *))
{
    if (fail_context_key_create)
        return EAGAIN;
    return pthread_key_create (key, destroy);
}
static int reply_write_error;
static char * directory_spool_data;
static size_t directory_spool_length, directory_spool_capacity;
static int directory_spool_create_error, directory_spool_write_error, directory_spool_read_error;
static unsigned directory_spool_opens, directory_spool_closes;
static void * directory_allocation;
static int path_error, open_error, handshake_error, version_error, mount_error;
static unsigned opens, closes, mounts;
static int transport_flags;
static int mount_saw_cleanup_storage;
static int mounted_flags;
static uint64_t mounted_writable;
static struct usfs_reply_header captured_reply;
static struct usfs_statfs_out captured_statistics;
static struct usfs_create_out captured_create;
static struct usfs_entry_out captured_entry;
static struct usfs_write_out captured_write;
static void namespace_test_reply (const struct usfs_reply_header * reply);
static void namespace_test_waiting (int exclusive) __attribute__ ((unused));
static void append_test_waiting (uint64_t id) __attribute__ ((unused));
static char loop_message[sizeof (struct usfs_in_hdr) + sizeof (struct usfs_open_in)];
static int loop_message_sent, loop_read_error, loop_eof;
static int loop_test_poll (struct pollfd * fds, unsigned long count, int timeout)
{
    (void)timeout;
    for (unsigned long i = 0; i < count; ++i)
        fds[i].revents = 0;
    sched_yield ();
    return 0;
}
ssize_t read (int fd, void * data, size_t size)
{
    if (fd == 73 && loop_eof)
        return 0;
    if (fd == 73 && loop_read_error)
    {
        errno = loop_read_error;
        return -1;
    }
    if (fd == 73 && !loop_message_sent)
    {
        if (size < sizeof (loop_message))
            abort ();
        memcpy (data, loop_message, sizeof (loop_message));
        loop_message_sent = 1;
        return sizeof (loop_message);
    }
    errno = EAGAIN;
    return -1;
}

ssize_t write (int fd, const void * data, size_t size)
{
    if (fd == 75 && size == 1)
        return 1;
    if (fd != 73 || size < sizeof (captured_reply))
        abort ();
    if (reply_write_error)
    {
        errno = reply_write_error;
        return -1;
    }
    memcpy (&captured_reply, data, sizeof (captured_reply));
    namespace_test_reply (&captured_reply);
    memset (&captured_statistics, 0, sizeof (captured_statistics));
    if (size == sizeof (captured_reply) + sizeof (captured_statistics))
        memcpy (&captured_statistics, (const char *)data + sizeof (captured_reply), sizeof (captured_statistics));
    if (size == sizeof (captured_reply) + sizeof (captured_create))
        memcpy (&captured_create, (const char *)data + sizeof (captured_reply), sizeof (captured_create));
    if (size == sizeof (captured_reply) + sizeof (captured_entry))
        memcpy (&captured_entry, (const char *)data + sizeof (captured_reply), sizeof (captured_entry));
    if (size == sizeof (captured_reply) + sizeof (captured_write))
        memcpy (&captured_write, (const char *)data + sizeof (captured_reply), sizeof (captured_write));
    return (ssize_t)size;
}

static void * track_allocation (void * pointer)
{
    if (pointer != NULL && allocation_tracking)
    {
        unsigned i;
        for (i = 0; i < 4; ++i)
        {
            if (mount_allocations[i] == NULL)
            {
                mount_allocations[i] = pointer;
                ++live_allocations;
                return pointer;
            }
        }
        abort ();
    }
    return pointer;
}

static void * mount_test_calloc (size_t count, size_t size)
{
    if (fail_client_calloc && --fail_client_calloc == 0)
        return NULL;
    if (fail_mount_calloc)
    {
        fail_mount_calloc = 0;
        errno = ENOMEM;
        return NULL;
    }
    return track_allocation (calloc (count, size));
}

static char * mount_test_strdup (const char * text)
{
    if (fail_mount_strdup)
    {
        fail_mount_strdup = 0;
        errno = ENOMEM;
        return NULL;
    }
    return track_allocation (strdup (text));
}

static void * mount_test_realloc (void * pointer, size_t size)
{
    if (directory_fail_growth)
    {
        void * result;
        if (++directory_growths == directory_fail_growth)
            return NULL;
        result = realloc (pointer, size);
        if (result != NULL)
            directory_allocation = result;
        return result;
    }
    if (fail_mount_realloc)
    {
        fail_mount_realloc = 0;
        errno = ENOMEM;
        return NULL;
    }
    return realloc (pointer, size);
}

static void mount_test_free (void * pointer)
{
    unsigned i;
    if (pointer == directory_allocation)
        directory_allocation = NULL;
    for (i = 0; i < 4; ++i)
    {
        if (pointer != NULL && mount_allocations[i] == pointer)
        {
            mount_allocations[i] = NULL;
            --live_allocations;
            break;
        }
    }
    free (pointer);
}

char * realpath (const char * path, char * resolved)
{
    (void)path;
    if (path_error)
    {
        errno = ENOENT;
        return NULL;
    }
    return strcpy (resolved, "/canonical/mount");
}

int open (const char * path, int flags, ...)
{
    if (strstr (path, "/wake") != NULL)
        return (flags & O_ACCMODE) == O_RDONLY ? 74 : 75;
    (void)path;
    (void)flags;
    transport_flags = flags;
    if (open_error)
    {
        errno = EACCES;
        return -1;
    }
    ++opens;
    return 73;
}

int close (int fd)
{
    if (fd == 76)
    {
        ++directory_spool_closes;
        return 0;
    }
    if (fd == 74 || fd == 75)
        return 0;
    if (fd != 73)
        abort ();
    ++closes;
    return 0;
}

int mkstemp (char * path)
{
    (void)path;
    if (directory_spool_create_error)
    {
        errno = EIO;
        return -1;
    }
    ++directory_spool_opens;
    directory_spool_length = 0;
    return 76;
}

int fcntl (int fd, int command, ...)
{
    if (fd != 76 || command != F_SETFD)
        abort ();
    return 0;
}

ssize_t pwrite (int fd, const void * data, size_t length, off_t offset)
{
    if (fd != 76 || offset < 0)
        abort ();
    if (directory_spool_write_error)
    {
        errno = EIO;
        return -1;
    }
    const size_t end = (size_t)offset + length;
    if (end > directory_spool_capacity)
    {
        size_t capacity = directory_spool_capacity == 0 ? 4096 : directory_spool_capacity;
        while (capacity < end)
            capacity *= 2;
        char * storage = realloc (directory_spool_data, capacity);
        if (storage == NULL)
        {
            errno = ENOMEM;
            return -1;
        }
        directory_spool_data = storage;
        directory_spool_capacity = capacity;
    }
    memcpy (directory_spool_data + offset, data, length);
    if (end > directory_spool_length)
        directory_spool_length = end;
    return (ssize_t)length;
}

ssize_t pread (int fd, void * data, size_t length, off_t offset)
{
    if (fd != 76 || offset < 0)
        abort ();
    if (directory_spool_read_error)
    {
        errno = EIO;
        return -1;
    }
    if ((size_t)offset >= directory_spool_length)
        return 0;
    if (length > directory_spool_length - (size_t)offset)
        length = directory_spool_length - (size_t)offset;
    memcpy (data, directory_spool_data + offset, length);
    return (ssize_t)length;
}

char * mkdtemp (char * path)
{
    return path;
}
int mkfifo (const char * path, mode_t mode)
{
    (void)path;
    (void)mode;
    return 0;
}
int unlink (const char * path)
{
    (void)path;
    return 0;
}
int rmdir (const char * path)
{
    (void)path;
    return 0;
}

int ioctl (int fd, int command, ...)
{
    va_list arguments;
    struct usfs_dev_info * info;
    if (fd != 73 || command != USFS_IOC_CONNECTION_REQUEST)
        abort ();
    if (handshake_error)
    {
        errno = EIO;
        return -1;
    }
    va_start (arguments, command);
    info = va_arg (arguments, struct usfs_dev_info *);
    va_end (arguments);
    memset (info, 0, sizeof (*info));
    info->protocol_version = USFS_PROTOCOL_VERSION + version_error;
    info->fs_type = 37;
    info->cookie = 123;
    return 0;
}

int vmount (struct vmount * mount, int length)
{
    (void)length;
    mounted_flags = mount->vmt_flags;
    if (usfs_info_field_u64 ((char *)mount + mount->vmt_data[VMT_INFO].vmt_off, mount->vmt_data[VMT_INFO].vmt_size, "rw", 2, 10, &mounted_writable) !=
        1)
        abort ();
    ++mounts;
    mount_saw_cleanup_storage = live_allocations == 2;
    if (mount_error)
    {
        errno = EBUSY;
        return -1;
    }
    return 0;
}

/* Execute the complete client implementation with allocation substitutes;
 * syscall substitutes above cannot mount or open a real kernel connection. */
#define calloc                            mount_test_calloc
#define pthread_key_create                context_test_key_create
#define strdup                            mount_test_strdup
#define realloc                           mount_test_realloc
#define free                              mount_test_free
#define USFS_NAMESPACE_WAITING(exclusive) namespace_test_waiting (exclusive)
#define USFS_OBJECT_WAITING(id)           append_test_waiting (id)
#define poll                              loop_test_poll
#define sigaction(...)                    signal_test_sigaction (__VA_ARGS__)
#include "../../../src/client/lib/usfs_client.c"
#undef sigaction
#undef poll
#undef pthread_key_create
#undef calloc
#undef strdup
#undef realloc
#undef free
#undef USFS_NAMESPACE_WAITING
#undef USFS_OBJECT_WAITING

static char mount_records[4096];
static size_t mount_records_size;
static int mount_records_count, query_error, query_grows;
static unsigned query_calls, unmount_calls;
static int unmounted_number;
static int unmount_error, unmount_removes_record;
static unsigned retry_sleeps, destroy_calls;

int usleep (unsigned int duration)
{
    (void)duration;
    ++retry_sleeps;
    return 0;
}

int mntctl (int command, size_t size, char * buffer)
{
    if (command != MCTL_QUERY)
        abort ();
    ++query_calls;
    if (query_error)
    {
        errno = EIO;
        return -1;
    }
    if (query_grows && query_calls == 1)
    {
        *(int *)buffer = 16384;
        return 0;
    }
    if (size < mount_records_size)
        abort ();
    memcpy (buffer, mount_records, mount_records_size);
    return mount_records_count;
}

int uvmount (int number, int flags)
{
    if (flags != 0)
        abort ();
    ++unmount_calls;
    unmounted_number = number;
    if (unmount_removes_record)
        ((struct vmount *)mount_records)->vmt_gfstype = 99;
    if (unmount_error)
    {
        errno = unmount_error;
        return -1;
    }
    return 0;
}

static struct vmount * add_mount_record (int number, uint64_t cookie, unsigned channel, const char * path)
{
    struct vmount * mount = (struct vmount *)(mount_records + mount_records_size);
    char * data = (char *)mount + sizeof (*mount);
    char info[128];
    memset (mount, 0, sizeof (*mount));
    mount->vmt_revision = VMT_REVISION;
    mount->vmt_gfstype = 37;
    mount->vmt_vfsnumber = number;
    snprintf (info, sizeof (info), "fd=73,chan=%u,cookie=%016llx,rw=0", channel, (unsigned long long)cookie);
    append_mount_field (mount, &data, VMT_STUB, path);
    append_mount_field (mount, &data, VMT_INFO, info);
    align_mount_data_cursor (&data);
    mount->vmt_length = (unsigned)(data - (char *)mount);
    mount_records_size += mount->vmt_length;
    ++mount_records_count;
    return mount;
}


static int last_native_result;

static struct usfs_client * new_test_client (const struct usfs_operations * operations, size_t operations_size, void * application_data)
{
    struct usfs_client * client = NULL;
    last_native_result = usfs_client_create (USFS_CLIENT_API_VERSION, operations, operations_size, NULL, application_data, &client);
    return client;
}

static void test_mount_identity (struct tap_state * tap)
{
    unsigned scenario;
    const char * names[] = { "old cleanup leaves a replacement at the same pathname untouched",
                             "owning connection is found after the mountpoint pathname changes",
                             "stacked replacement preceding the owner is not selected",
                             "stacked replacement following the owner is not selected",
                             "cookie prefix collision cannot identify the owning connection",
                             "matching cookie on another channel is not the owning mount",
                             "malformed identity metadata cannot authorize unmount",
                             "ambiguous owning identities cannot authorize unmount",
                             "failed mount query preserves cleanup ownership",
                             "grown mount query still selects only the owning mount number" };
    for (scenario = 0; scenario < 10; ++scenario)
    {
        struct usfs_client f = { 0 };
        struct vmount * record;
        int expected = 0;
        f.mounted = 1;
        f.mountpoint = (char *)"/canonical/mount";
        f.info.fs_type = 37;
        f.info.cookie = 123;
        mount_records_size = 0;
        mount_records_count = query_error = query_grows = 0;
        query_calls = unmount_calls = 0;
        unmounted_number = -1;
        switch (scenario)
        {
            case 0:
                add_mount_record (22, 456, 0, f.mountpoint);
                break;
            case 1:
                add_mount_record (11, 123, 0, "/renamed/mount");
                expected = 1;
                break;
            case 2:
                add_mount_record (22, 456, 0, f.mountpoint);
                add_mount_record (11, 123, 0, f.mountpoint);
                expected = 1;
                break;
            case 3:
                add_mount_record (11, 123, 0, f.mountpoint);
                add_mount_record (22, 456, 0, f.mountpoint);
                expected = 1;
                break;
            case 4:
                add_mount_record (22, 0x7b00, 0, f.mountpoint);
                break;
            case 5:
                add_mount_record (22, 123, 1, f.mountpoint);
                break;
            case 6:
                record = add_mount_record (11, 123, 0, f.mountpoint);
                record->vmt_data[VMT_INFO].vmt_off = -1;
                break;
            case 7:
                add_mount_record (11, 123, 0, f.mountpoint);
                add_mount_record (22, 123, 0, f.mountpoint);
                break;
            case 8:
                query_error = 1;
                break;
            case 9:
                query_grows = 1;
                add_mount_record (11, 123, 0, f.mountpoint);
                expected = 1;
                break;
        }
        (last_native_result = usfs_client_unmount (&f));
        tap_ok (
            tap,
            unmount_calls == (unsigned)expected && (!expected || unmounted_number == 11) && ((scenario < 6 || scenario > 8) || f.mounted) &&
                (scenario != 9 || query_calls == 2),
            names[scenario]
        );
    }
}

static void observe_destroy (const struct usfs_client_request * data)
{
    (void)data;
    ++destroy_calls;
}

static void test_unmount_retry (struct tap_state * tap)
{
    struct usfs_client f = { 0 };
    struct usfs_operations operations = { 0 };
    struct usfs_client * allocated;
    int refused;
    f.fd = 73;
    f.mounted = 1;
    f.mountpoint = (char *)"/canonical/mount";
    f.info.fs_type = 37;
    f.info.cookie = 123;
    mount_records_size = 0;
    mount_records_count = query_error = query_grows = 0;
    query_calls = unmount_calls = retry_sleeps = closes = 0;
    add_mount_record (11, 123, 0, f.mountpoint);
    unmount_error = EBUSY;
    (last_native_result = usfs_client_unmount (&f));
    tap_ok (
        tap,
        f.mounted && f.fd == 73 && last_native_result == -EBUSY && unmount_calls == 20 && closes == 0,
        "exhausted busy retries retain mount and descriptor ownership"
    );
    unmount_error = 0;
    (last_native_result = usfs_client_unmount (&f));
    tap_ok (
        tap,
        !f.mounted && last_native_result == 0 && unmount_calls == 21,
        "same instance retries successfully after busy ownership is released"
    );
    f.mounted = 1;
    unmount_error = EIO;
    (last_native_result = usfs_client_unmount (&f));
    tap_ok (tap, f.mounted && last_native_result == -EIO && f.fd == 73 && closes == 0, "non-busy unmount error retains ownership for retry");
    unmount_error = 0;
    fail_mount_realloc = 1;
    (last_native_result = usfs_client_unmount (&f));
    tap_ok (tap, f.mounted && last_native_result == -ENOMEM && f.fd == 73, "mount-query allocation failure retains retryable ownership");
    (last_native_result = usfs_client_unmount (&f));
    tap_ok (tap, !f.mounted && last_native_result == 0, "mount-query allocation failure can be retried through the same instance");
    f.mounted = 1;
    unmount_error = EINVAL;
    (last_native_result = usfs_client_unmount (&f));
    tap_ok (tap, f.mounted && last_native_result == -EINVAL, "removal race is not assumed to prove mount absence");
    unmount_removes_record = 1;
    (last_native_result = usfs_client_unmount (&f));
    tap_ok (tap, !f.mounted && last_native_result == 0, "external removal during cleanup is reconciled by verified absence");
    unmount_removes_record = 0;
    f.mounted = 1;
    mount_records_size = 0;
    mount_records_count = 0;
    add_mount_record (22, 456, 0, f.mountpoint);
    unmount_error = 0;
    (last_native_result = usfs_client_unmount (&f));
    tap_ok (tap, !f.mounted && last_native_result == 0, "subsequent verified absence completes cleanup after removal race");

    operations.shutdown = observe_destroy;
    allocated = new_test_client (&operations, sizeof (operations), NULL);
    if (allocated == NULL)
        abort ();
    allocated->fd = 73;
    allocated->mounted = 1;
    allocated->mountpoint = strdup ("/canonical/mount");
    if (allocated->mountpoint == NULL)
        abort ();
    allocated->init_done = 1;
    allocated->info = f.info;
    closes = destroy_calls = 0;
    errno = 0;
    (last_native_result = usfs_client_destroy (&allocated));
    refused = destroy_calls == 0 && closes == 0 && last_native_result == -EBUSY;
    tap_ok (tap, refused, "destruction reports unresolved cleanup without releasing live ownership");
    /* On the preceding revision, destroy already freed the object. Inspect
     * only external callback/descriptor counters and never reuse that pointer. */
    if (refused)
    {
        (last_native_result = usfs_client_unmount (allocated));
        tap_ok (tap, !allocated->mounted && last_native_result == 0, "refused destruction leaves an instance that can retry cleanup");
        (last_native_result = usfs_client_destroy (&allocated));
        tap_ok (tap, destroy_calls == 1 && closes == 1, "final destruction releases backend and connection exactly once");
    }
    else
    {
        tap_ok (tap, 0, "refused destruction leaves an instance that can retry cleanup");
        tap_ok (tap, 0, "final destruction releases backend and connection exactly once");
    }
}

static void reset_mount_services (void)
{
    path_error = open_error = handshake_error = version_error = mount_error = 0;
    fail_mount_calloc = fail_mount_strdup = 0;
    opens = closes = mounts = 0;
    mount_saw_cleanup_storage = 0;
}

static void test_mount_ownership (struct tap_state * tap)
{
    struct usfs_operations operations = { 0 };
    struct usfs_client * f = new_test_client (&operations, sizeof (operations), NULL);
    unsigned scenario;
    const char * names[] = { "bad mountpoint leaves no descriptor or allocation",
                             "failed device open leaves no cleanup allocation",
                             "failed handshake closes the acquired descriptor",
                             "protocol mismatch closes the acquired descriptor",
                             "mountpoint allocation failure cannot publish an untracked mount",
                             "mount buffer allocation failure closes and frees acquired resources",
                             "failed vmount frees all prepared cleanup state" };
    if (f == NULL)
        abort ();
    allocation_tracking = 1;
    for (scenario = 0; scenario < 7; ++scenario)
    {
        reset_mount_services ();
        switch (scenario)
        {
            case 0:
                path_error = 1;
                break;
            case 1:
                open_error = 1;
                break;
            case 2:
                handshake_error = 1;
                break;
            case 3:
                version_error = -1;
                break;
            case 4:
                fail_mount_strdup = 1;
                break;
            case 5:
                fail_mount_calloc = 1;
                break;
            case 6:
                mount_error = 1;
                break;
        }
        int rc = usfs_client_mount (f, "/mount");
        tap_ok (
            tap,
            rc < 0 && f->fd == -1 && !f->mounted && f->mountpoint == NULL && opens == closes && live_allocations == 0 &&
                mounts == (scenario == 6 ? 1u : 0u),
            names[scenario]
        );
        /* Safely reset the simulated mount after the preceding-revision
         * failure: the regression never touches the real mount table. */
        if (f->mounted)
        {
            f->mounted = 0;
            if (f->fd >= 0)
            {
                close (f->fd);
                f->fd = -1;
            }
        }
    }
    reset_mount_services ();
    tap_ok (
        tap,
        usfs_client_mount (f, "/mount") == 0 && f->mounted && f->fd == 73 && f->mountpoint != NULL && strcmp (f->mountpoint, "/canonical/mount") == 0,
        "same client successfully retries after pre-mount failures"
    );
    tap_ok (
        tap,
        mounts == 1 && mount_saw_cleanup_storage && live_allocations == 1 && opens == 1 && closes == 0 &&
            (transport_flags & (O_NONBLOCK | _FCLOEXEC)) == (O_NONBLOCK | _FCLOEXEC),
        "successful vmount already owns its canonical cleanup state"
    );
    f->mounted = 0; /* Simulate successful removal of the test-only mount. */
    (last_native_result = usfs_client_destroy (&f));
    tap_ok (tap, live_allocations == 0 && closes == 1, "destruction releases prepared state and the connection exactly once");
    allocation_tracking = 0;
}

static int writable_utimens (
    const struct usfs_client_request * callback_request,
    const char * path,
    const struct timespec times[2],
    struct usfs_open_file * info
)
{
    (void)callback_request;
    (void)path;
    (void)times;
    (void)info;
    return 0;
}

static void test_mount_access (struct tap_state * tap)
{
    const enum usfs_mount_access modes[] = { USFS_MOUNT_AUTOMATIC, USFS_MOUNT_READ_ONLY, USFS_MOUNT_READ_WRITE };
    struct usfs_operations operations = { .utimens = writable_utimens };
    for (unsigned index = 0; index < sizeof (modes) / sizeof (modes[0]); ++index)
    {
        struct usfs_client_options options = { 0 };
        struct usfs_client * client = NULL;
        options.mount_access = modes[index];
        reset_mount_services ();
        int rc = usfs_client_create (USFS_CLIENT_API_VERSION, &operations, sizeof (operations), &options, NULL, &client);
        if (rc == 0)
            rc = usfs_client_mount (client, "/mount");
        const int writable = modes[index] != USFS_MOUNT_READ_ONLY;
        const int valid = rc == 0 && mounted_writable == (uint64_t)writable && !!(mounted_flags & MNT_READONLY) == !writable;
        if (client != NULL)
        {
            client->mounted = 0;
            (void)usfs_client_destroy (&client);
        }
        tap_ok (tap, valid && opens == closes, "native access selection reaches AIX flags and USFS admission");
    }
}

static struct statvfs backend_statistics;
static int backend_statistics_error;

static int statistics_callback (const struct usfs_client_request * callback_request, const char * path, struct statvfs * statistics)
{
    (void)callback_request;
    (void)path;
    *statistics = backend_statistics;
    return backend_statistics_error;
}

static void test_statistics_reply (struct tap_state * tap)
{
    struct usfs_operations operations = { 0 };
    struct usfs_client * f = new_test_client (&operations, sizeof (operations), NULL);
    struct usfs_in_hdr request = { 0 };
    struct request_context context = { 0 };

    context.header = &request;

    request.nodeid = USFS_ROOT_ID;
    f->fd = 73;
    context.client = f;
    tap_ok (
        tap,
        handle_statfs_request (&context) == 0 && captured_reply.error == ENOSYS,
        "absent statistics callback returns an error without capacity"
    );
    f->ops.statfs = statistics_callback;
    backend_statistics_error = -ENOSPC;
    tap_ok (tap, handle_statfs_request (&context) == 0 && captured_reply.error == ENOSPC, "statistics callback error is preserved in the reply");
    backend_statistics_error = 0;
    backend_statistics.f_bsize = 4096;
    backend_statistics.f_namemax = 255;
    tap_ok (
        tap,
        handle_statfs_request (&context) == 0 && captured_reply.error == 0 && captured_statistics.blocks == 0 && captured_statistics.bavail == 0,
        "client preserves authoritative zero capacity"
    );
    backend_statistics.f_blocks = 7;
    tap_ok (
        tap,
        handle_statfs_request (&context) == 0 && captured_reply.error == 0 && captured_statistics.blocks == 7 && captured_statistics.bfree == 0,
        "client preserves authoritative full capacity"
    );
    backend_statistics.f_bfree = 8;
    tap_ok (
        tap,
        handle_statfs_request (&context) == 0 && captured_reply.error == EIO,
        "client rejects inconsistent backend capacity before replying"
    );
    backend_statistics.f_bfree = 0;
    backend_statistics.f_frsize = (uint64_t)UINT32_MAX + 4097u;
    tap_ok (
        tap,
        handle_statfs_request (&context) == 0 && captured_reply.error == EIO,
        "client rejects block size narrowing instead of changing capacity units"
    );
    backend_statistics.f_frsize = 4096;
    backend_statistics.f_namemax = (uint64_t)UINT32_MAX + 256u;
    tap_ok (tap, handle_statfs_request (&context) == 0 && captured_reply.error == EIO, "client rejects name limit narrowing before replying");
    (last_native_result = usfs_client_destroy (&f));
}

static int backend_read_result;
static const char read_fixture[] = "data";
static size_t backend_read_bytes_written = sizeof (read_fixture) - 1u;

static ssize_t bounded_read_callback (
    const struct usfs_client_request * callback_request,
    const char * path,
    char * data,
    size_t requested_size,
    off_t offset,
    struct usfs_open_file * file_info
)
{
    (void)callback_request;
    (void)path;
    (void)offset;
    (void)file_info;

    if (requested_size >= backend_read_bytes_written)
        memcpy (data, read_fixture, backend_read_bytes_written);

    return backend_read_result;
}

static void test_read_reply_bounds (struct tap_state * tap)
{
    struct usfs_operations operations = { 0 };
    struct usfs_in_hdr request_header = { 0 };
    struct usfs_read_in read_request = { 0 };
    struct request_context context = { 0 };
    char data[sizeof (read_fixture) - 1] = { 0 };

    operations.read = bounded_read_callback;

    struct usfs_client * client = new_test_client (&operations, sizeof (operations), NULL);
    if (client == NULL)
        abort ();

    client->fd = 73;
    context.client = client;
    context.header = &request_header;
    read_request.size = sizeof (data);

    backend_read_result = 0;
    tap_ok (
        tap,
        read_file_and_reply (&context, "/file", &read_request, data) == 0 && captured_reply.error == 0 &&
            captured_reply.len == sizeof (captured_reply),
        "zero-length backend read sends an empty reply"
    );

    backend_read_result = (int)sizeof (data);
    tap_ok (
        tap,
        read_file_and_reply (&context, "/file", &read_request, data) == 0 && captured_reply.error == 0 &&
            captured_reply.len == sizeof (captured_reply) + sizeof (data),
        "backend read may fill the requested size"
    );

    memset (data, 'S', sizeof (data));
    backend_read_bytes_written = 1;
    tap_ok (
        tap,
        read_file_and_reply (&context, "/file", &read_request, data) == 0 && captured_reply.error == 0 && data[0] == 'd' && data[1] == 0 &&
            data[2] == 0 && data[3] == 0,
        "under-writing backend cannot expose bytes from an earlier reply"
    );
    backend_read_bytes_written = sizeof (read_fixture) - 1u;

    backend_read_result = (int)sizeof (data) + 1;
    tap_ok (
        tap,
        read_file_and_reply (&context, "/file", &read_request, data) == 0 && captured_reply.error == EIO &&
            captured_reply.len == sizeof (captured_reply),
        "over-reported backend read sends EIO without extra bytes"
    );

    backend_read_result = (int)USFS_MAX_DATA + 1;
    tap_ok (
        tap,
        read_file_and_reply (&context, "/file", &read_request, data) == 0 && captured_reply.error == EIO &&
            captured_reply.len == sizeof (captured_reply),
        "backend read beyond the protocol maximum cannot overrun the reply buffer"
    );

    backend_read_result = INT_MAX;
    tap_ok (
        tap,
        read_file_and_reply (&context, "/file", &read_request, data) == 0 && captured_reply.error == EIO &&
            captured_reply.len == sizeof (captured_reply),
        "extreme backend read count cannot overrun the reply buffer"
    );

    backend_read_result = -EIO;
    tap_ok (
        tap,
        read_file_and_reply (&context, "/file", &read_request, data) == 0 && captured_reply.error == EIO,
        "negative backend read error is preserved"
    );

    backend_read_result = (int)sizeof (data);
    tap_ok (
        tap,
        read_file_and_reply (&context, "/file", &read_request, data) == 0 && captured_reply.error == 0,
        "valid read still succeeds after an invalid callback result"
    );

    tap_ok (
        tap,
        send_reply (&context, 0, data, USFS_MSG_MAX) == 0 && captured_reply.error == EIO && captured_reply.len == sizeof (captured_reply),
        "reply construction rejects a body larger than its buffer"
    );

    tap_ok (
        tap,
        send_reply (&context, 0, NULL, 1) == 0 && captured_reply.error == EIO && captured_reply.len == sizeof (captured_reply),
        "reply construction rejects a missing body"
    );

    (last_native_result = usfs_client_destroy (&client));
}

static int terminate_readlink_result;

static int bounded_readlink_callback (const struct usfs_client_request * callback_request, const char * path, char * link, size_t capacity)
{
    (void)callback_request;
    (void)path;

    memset (link, 'x', capacity);
    if (terminate_readlink_result)
        link[capacity - 1u] = '\0';

    return 0;
}

static void test_readlink_termination (struct tap_state * tap)
{
    struct usfs_operations operations = { 0 };
    struct usfs_in_hdr request_header = { 0 };
    struct request_context context = { 0 };

    operations.readlink = bounded_readlink_callback;

    struct usfs_client * client = new_test_client (&operations, sizeof (operations), NULL);
    if (client == NULL)
        abort ();

    client->fd = 73;
    request_header.nodeid = USFS_ROOT_ID;
    context.client = client;
    context.header = &request_header;

    terminate_readlink_result = 0;
    tap_ok (
        tap,
        handle_readlink_request (&context) == 0 && captured_reply.error == EIO && captured_reply.len == sizeof (captured_reply),
        "unterminated readlink result is rejected without reading past the callback buffer"
    );

    terminate_readlink_result = 1;
    tap_ok (
        tap,
        handle_readlink_request (&context) == 0 && captured_reply.error == 0 && captured_reply.len == sizeof (captured_reply) + USFS_MAX_LINK - 1u,
        "maximum terminated readlink result remains accepted"
    );

    (last_native_result = usfs_client_destroy (&client));
}

static int created_flags, opened_flags, released_flags;
static unsigned create_calls, open_calls, release_calls;
static uint64_t released_handle;

static int flags_create (const struct usfs_client_request * callback_request, const char * path, mode_t mode, struct usfs_open_file * fi)
{
    (void)callback_request;
    (void)path;
    (void)mode;
    ++create_calls;
    created_flags = fi->open_flags;
    fi->value = 123;
    return 0;
}

static int flags_open (const struct usfs_client_request * callback_request, const char * path, struct usfs_open_file * fi)
{
    (void)callback_request;
    (void)path;
    ++open_calls;
    opened_flags = fi->open_flags;
    fi->value = 456;
    return 0;
}

static int flags_release (const struct usfs_client_request * callback_request, const char * path, struct usfs_open_file * fi)
{
    (void)callback_request;
    (void)path;
    ++release_calls;
    released_flags = fi->open_flags;
    released_handle = fi->value;
    return 0;
}

static void test_create_flags (struct tap_state * tap)
{
    const struct
    {
        uint32_t flags;
        int expected;
        const char * name;
    } cases[] = {
        { 1 | O_CREAT | O_EXCL, O_RDONLY, "read-only CREATE and RELEASE preserve access flags" },
        { 2 | O_CREAT | O_TRUNC, O_WRONLY, "write-only CREATE and RELEASE preserve access flags" },
        { 3 | O_CREAT, O_RDWR, "read-write CREATE and RELEASE preserve access flags" },
        { 2 | O_CREAT | O_APPEND | O_NONBLOCK, O_WRONLY | O_APPEND | O_NONBLOCK, "CREATE OPEN and RELEASE preserve append and nonblocking flags" },
        { O_CREAT, -EINVAL, "CREATE without access mode fails before the callback" },
        { 1 | O_CREAT | O_TRUNC, -EINVAL, "read-only truncating CREATE fails before the callback" },
        { 3 | O_CREAT | O_SYNC, -EOPNOTSUPP, "unsupported synchronous CREATE fails before mutation" },
        { 3 | O_CREAT | _FDIRECT, -EOPNOTSUPP, "unsupported direct-I/O CREATE fails before mutation" },
        { _FEXEC | O_CREAT, -EINVAL, "CREATE rejects executable activation before mutation" }
    };
    unsigned i;
    struct usfs_operations operations = { 0 };
    operations.create = flags_create;
    operations.open = flags_open;
    operations.release = flags_release;
    for (i = 0; i < sizeof (cases) / sizeof (cases[0]); ++i)
    {
        struct usfs_client * f = new_test_client (&operations, sizeof (operations), NULL);
        struct usfs_in_hdr request = { 0 };
        struct request_context context = { 0 };

        context.header = &request;

        struct usfs_create_attr_in create = { 0 };
        struct usfs_open_in open_body = { 0 };
        struct usfs_release_in release = { 0 };
        char payload[sizeof (create) + 5];
        int ok;
        f->fd = 73;
        context.client = f;
        request.nodeid = USFS_ROOT_ID;
        create.flags = cases[i].flags;
        create.activation = USFS_CREATE_OPEN;
        create.attr.valid = USFS_SET_MODE;
        create.attr.mode = 0600;
        memcpy (payload, &create, sizeof (create));
        memcpy (payload + sizeof (create), "file", 5);
        create_calls = open_calls = release_calls = 0;
        ok = handle_create_attr_request (&context, payload, sizeof (payload)) == 0;
        if (cases[i].expected < 0)
        {
            ok &= captured_reply.error == -cases[i].expected && create_calls == 0;
        }
        else
        {
            ok &= captured_reply.error == 0 && create_calls == 1 && created_flags == cases[i].expected;
            request.nodeid = captured_create.nodeid;
            release.fh = captured_create.fh;
            release.flags = cases[i].flags;
            ok &= handle_release_request (&context, (char *)&release, sizeof (release)) == 0 && release_calls == 1 &&
                  released_flags == created_flags && released_handle == 123;
            open_body.flags = cases[i].flags;
            ok &= handle_open_request (&context, (char *)&open_body, sizeof (open_body)) == 0 && captured_reply.error == 0 && open_calls == 1 &&
                  opened_flags == created_flags;
            release.fh = 456;
            ok &= handle_release_request (&context, (char *)&release, sizeof (release)) == 0 && release_calls == 2 && released_handle == 456 &&
                  released_flags == opened_flags;
        }
        tap_ok (tap, ok, cases[i].name);
        if (i == 0)
        {
            open_body.flags = _FEXEC | _FLARGEFILE | _FKERNEL;
            release.flags = open_body.flags;
            release.fh = 456;
            tap_ok (
                tap,
                handle_open_request (&context, (char *)&open_body, sizeof (open_body)) == 0 && captured_reply.error == 0 &&
                    opened_flags == O_RDONLY && handle_release_request (&context, (char *)&release, sizeof (release)) == 0 &&
                    captured_reply.error == 0 && released_flags == O_RDONLY,
                "AIX executable OPEN and RELEASE retain a readable backend handle"
            );
        }
        (last_native_result = usfs_client_destroy (&f));
    }
}

/* These barriers observe admission or callback entry, never elapsed sleeps.
 * The same scenarios run against the preceding client: without the gate the
 * callback itself advances the event and exposes the wrong ordering/path. */
static pthread_mutex_t ns_test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ns_test_changed = PTHREAD_COND_INITIALIZER;
static struct
{
    int active, moved, reads, reader_events, writer_events, rename_entered;
    int release_read, release_rename, release_reply, reply_entered;
    int block_read, block_reply, bad_paths, errors;
} ns_test;

static void ns_wait_for (int * value, int minimum)
{
    struct timespec deadline;
    deadline.tv_sec = time (NULL) + 15;
    deadline.tv_nsec = 0;
    while (*value < minimum)
    {
        if (pthread_cond_timedwait (&ns_test_changed, &ns_test_lock, &deadline) != 0)
        {
            fprintf (stderr, "namespace regression barrier timed out\n");
            _Exit (2);
        }
    }
}

static void namespace_test_waiting (int exclusive)
{
    pthread_mutex_lock (&ns_test_lock);
    if (ns_test.active)
    {
        if (exclusive)
            ++ns_test.writer_events;
        else
            ++ns_test.reader_events;
        pthread_cond_broadcast (&ns_test_changed);
    }
    pthread_mutex_unlock (&ns_test_lock);
}

static void namespace_test_reply (const struct usfs_reply_header * reply)
{
    pthread_mutex_lock (&ns_test_lock);
    if (ns_test.active)
    {
        if (reply->error != 0)
            ++ns_test.errors;
        if (reply->id == 1 && ns_test.block_reply)
        {
            ns_test.reply_entered = 1;
            pthread_cond_broadcast (&ns_test_changed);
            ns_wait_for (&ns_test.release_reply, 1);
        }
    }
    pthread_mutex_unlock (&ns_test_lock);
}

static int ns_getattr (const struct usfs_client_request * callback_request, const char * path, struct stat * st, struct usfs_open_file * fi)
{
    (void)callback_request;
    (void)fi;
    pthread_mutex_lock (&ns_test_lock);
    ++ns_test.reads;
    ++ns_test.reader_events;
    if (strcmp (path, "/independent") != 0 && strcmp (path, ns_test.moved ? "/new/file" : "/old/file") != 0)
        ++ns_test.bad_paths;
    pthread_cond_broadcast (&ns_test_changed);
    if (ns_test.block_read)
        ns_wait_for (&ns_test.release_read, 1);
    pthread_mutex_unlock (&ns_test_lock);
    memset (st, 0, sizeof (*st));
    st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    return 0;
}

static int ns_rename (const struct usfs_client_request * callback_request, const char * oldpath, const char * newpath)
{
    (void)callback_request;
    pthread_mutex_lock (&ns_test_lock);
    if (strcmp (oldpath, "/old") || strcmp (newpath, "/new"))
        ++ns_test.bad_paths;
    ns_test.moved = ns_test.rename_entered = 1;
    ++ns_test.writer_events;
    pthread_cond_broadcast (&ns_test_changed);
    ns_wait_for (&ns_test.release_rename, 1);
    pthread_mutex_unlock (&ns_test_lock);
    return 0;
}

struct ns_job
{
    struct usfs_client * f;
    uint64_t id;
    int rename;
};

static void * ns_dispatch (void * argument)
{
    struct ns_job * job = argument;
    struct client_thread_context context = { 0 };
    char message[sizeof (struct usfs_in_hdr) + sizeof (struct usfs_rename_in) + 8];
    struct usfs_in_hdr request = { 0 };
    size_t size = sizeof (request);
    context.writebuf = malloc (USFS_MSG_MAX);
    if (context.writebuf == NULL || ensure_thread_context_key () != 0 || pthread_setspecific (client_context_key, &context) != 0)
        abort ();
    request.version = USFS_PROTOCOL_VERSION;
    request.opcode = job->rename == 3 ? USFS_OP_LOOKUP : job->rename == 2 ? 22 : job->rename ? USFS_OP_RENAME : USFS_OP_GETATTR;
    request.nodeid = job->id;
    request.unique = job->rename ? 1 : job->id;
    if (job->rename == 3)
    {
        memcpy (message + size, "independent", 12);
        size += 12;
    }
    else if (job->rename == 2)
    {
        uint64_t count = 1;
        memcpy (message + size, &count, sizeof (count));
        size += sizeof (count);
    }
    else if (job->rename)
    {
        struct usfs_rename_in rename = { 0 };
        rename.newparent = USFS_ROOT_ID;
        rename.oldnamelen = 4;
        memcpy (message + size, &rename, sizeof (rename));
        size += sizeof (rename);
        memcpy (message + size, "old\0new", 8);
        size += 8;
    }
    request.len = (uint32_t)size;
    memcpy (message, &request, sizeof (request));
    handle_request (job->f, message, size);
    pthread_setspecific (client_context_key, NULL);
    free (context.writebuf);
    return NULL;
}

static void ns_start (pthread_t * thread, struct ns_job * job)
{
    if (pthread_create (thread, NULL, ns_dispatch, job) != 0)
        abort ();
}

static void test_namespace_transactions (struct tap_state * tap)
{
    unsigned scenario;
    for (scenario = 0; scenario < 4; ++scenario)
    {
        struct usfs_operations ops = { 0 };
        struct usfs_client * f;
        struct client_node *parent, *child, *other;
        pthread_t writer, first, second;
        struct ns_job rename, read, independent;
        int ordered = 1;
        ops.getattr = ns_getattr;
        ops.rename = ns_rename;
        f = new_test_client (&ops, sizeof (ops), NULL);
        if (f == NULL)
            abort ();
        f->fd = 73;
        parent = find_or_create_node (f, USFS_ROOT_ID, "old", NULL);
        child = find_or_create_node (f, parent->id, "file", NULL);
        other = find_or_create_node (f, USFS_ROOT_ID, "independent", NULL);
        rename = (struct ns_job){ f, USFS_ROOT_ID, 1 };
        read = (struct ns_job){ f, child->id, 0 };
        independent = (struct ns_job){ f, other->id, 0 };
        memset (&ns_test, 0, sizeof (ns_test));
        ns_test.active = 1;
        ns_test.block_read = scenario == 1 || scenario == 3;
        ns_test.block_reply = scenario == 2;
        ns_test.release_rename = scenario >= 2;
        if (scenario == 1 || scenario == 3)
        {
            ns_start (&first, &read);
            pthread_mutex_lock (&ns_test_lock);
            ns_wait_for (&ns_test.reads, 1);
            pthread_mutex_unlock (&ns_test_lock);
        }
        if (scenario != 3)
        {
            ns_start (&writer, &rename);
            pthread_mutex_lock (&ns_test_lock);
            ns_wait_for (scenario == 1 ? &ns_test.writer_events : scenario == 2 ? &ns_test.reply_entered : &ns_test.rename_entered, 1);
            if (scenario == 1)
                ordered &= ns_test.rename_entered == 0;
            pthread_mutex_unlock (&ns_test_lock);
        }
        ns_start (&second, scenario == 3 ? &independent : &read);
        pthread_mutex_lock (&ns_test_lock);
        ns_wait_for (&ns_test.reader_events, scenario == 1 || scenario == 3 ? 2 : 1);
        if (scenario == 3)
            ordered &= ns_test.reads == 2;
        else
            ordered &= ns_test.reads == (scenario == 1 ? 1 : 0);
        ns_test.release_read = ns_test.release_rename = ns_test.release_reply = 1;
        pthread_cond_broadcast (&ns_test_changed);
        pthread_mutex_unlock (&ns_test_lock);
        if (scenario != 3)
            pthread_join (writer, NULL);
        if (scenario == 1 || scenario == 3)
            pthread_join (first, NULL);
        pthread_join (second, NULL);
        ordered &= ns_test.bad_paths == 0 && ns_test.errors == 0;
        ns_test.active = 0;
        tap_ok (
            tap,
            ordered,
            scenario == 0   ? "ancestor rename blocks child observations until identity publication"
            : scenario == 1 ? "queued namespace writer precedes new readers after existing observations finish"
            : scenario == 2 ? "namespace ownership includes successful reply preparation and submission"
                            : "independent observations overlap under shared namespace admission"
        );
        (last_native_result = usfs_client_destroy (&f));
    }
}

static struct
{
    int active, entered, events, release, second_mutations, bad_handles;
    size_t size[2];
    char data[2][8];
} append_test;

static void append_test_waiting (uint64_t id)
{
    (void)id;
    pthread_mutex_lock (&ns_test_lock);
    if (append_test.active)
    {
        ++append_test.events;
        pthread_cond_broadcast (&ns_test_changed);
    }
    pthread_mutex_unlock (&ns_test_lock);
}

static int append_getattr (const struct usfs_client_request * callback_request, const char * path, struct stat * st, struct usfs_open_file * fi)
{
    (void)callback_request;
    int actor = (int)get_callback_request ()->pid;
    int object = actor == 3;
    (void)path;
    memset (st, 0, sizeof (*st));
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 2;
    st->st_ino = object + 42;
    pthread_mutex_lock (&ns_test_lock);
    st->st_size = append_test.size[object];
    if (fi == NULL || fi->value != (uint64_t)(900 + actor))
        ++append_test.bad_handles;
    ++append_test.events;
    if (actor == 1)
        append_test.entered = 1;
    pthread_cond_broadcast (&ns_test_changed);
    if (actor == 1)
        ns_wait_for (&append_test.release, 1);
    pthread_mutex_unlock (&ns_test_lock);
    return 0;
}

static ssize_t append_write (
    const struct usfs_client_request * callback_request,
    const char * path,
    const char * data,
    size_t size,
    off_t offset,
    struct usfs_open_file * fi
)
{
    (void)callback_request;
    int actor = (int)get_callback_request ()->pid;
    int object = actor == 3;
    (void)path;
    if (size != 1 || offset < 0 || offset >= 8 || fi == NULL)
        return -EINVAL;
    pthread_mutex_lock (&ns_test_lock);
    append_test.data[object][offset] = *data;
    if (append_test.size[object] < (size_t)offset + size)
        append_test.size[object] = (size_t)offset + size;
    if (actor != 1)
        ++append_test.second_mutations;
    ++append_test.events;
    pthread_cond_broadcast (&ns_test_changed);
    pthread_mutex_unlock (&ns_test_lock);
    return 1;
}

static int append_truncate (const struct usfs_client_request * callback_request, const char * path, off_t size, struct usfs_open_file * fi)
{
    (void)callback_request;
    (void)path;
    (void)fi;
    pthread_mutex_lock (&ns_test_lock);
    append_test.size[0] = (size_t)size;
    ++append_test.second_mutations;
    ++append_test.events;
    pthread_cond_broadcast (&ns_test_changed);
    pthread_mutex_unlock (&ns_test_lock);
    return 0;
}

struct append_job
{
    struct usfs_client * f;
    uint64_t id;
    int actor, kind;
};
static void * append_dispatch (void * argument)
{
    struct append_job * job = argument;
    struct client_thread_context context = { 0 };
    char message[sizeof (struct usfs_in_hdr) + sizeof (struct usfs_setattr_in) + 1];
    struct usfs_in_hdr in = { 0 };
    size_t size = sizeof (in);
    context.writebuf = malloc (USFS_MSG_MAX);
    if (context.writebuf == NULL || ensure_thread_context_key () != 0 || pthread_setspecific (client_context_key, &context) != 0)
        abort ();
    in.version = USFS_PROTOCOL_VERSION;
    in.nodeid = job->id;
    in.unique = in.pid = (uint32_t)job->actor;
    if (job->kind == 2)
    {
        struct usfs_setattr_in change = { 0 };
        change.valid = USFS_SET_SIZE;
        change.fh = 900 + job->actor;
        in.opcode = USFS_OP_SETATTR;
        memcpy (message + size, &change, sizeof (change));
        size += sizeof (change);
    }
    else
    {
        struct usfs_write_in write = { 0 };
        write.fh = 900 + job->actor;
        write.flags = job->kind == 1 ? 0 : USFS_WRITE_APPEND;
        write.size = 1;
        in.opcode = USFS_OP_WRITE;
        memcpy (message + size, &write, sizeof (write));
        size += sizeof (write);
        message[size++] = job->actor == 1 ? 'A' : 'B';
    }
    in.len = (uint32_t)size;
    memcpy (message, &in, sizeof (in));
    handle_request (job->f, message, size);
    pthread_setspecific (client_context_key, NULL);
    free (context.writebuf);
    return NULL;
}

static void test_append_transactions (struct tap_state * tap)
{
    unsigned scenario;
    for (scenario = 0; scenario < 4; ++scenario)
    {
        struct usfs_operations ops = { 0 };
        struct usfs_client * f;
        struct client_node *a, *alias, *other;
        struct stat st = { 0 };
        struct append_job first, second;
        pthread_t one, two;
        int valid;
        ops.getattr = append_getattr;
        ops.write = append_write;
        ops.truncate = append_truncate;
        f = new_test_client (&ops, sizeof (ops), NULL);
        if (f == NULL)
            abort ();
        f->fd = 73;
        st.st_mode = S_IFREG | 0600;
        st.st_nlink = 2;
        st.st_ino = 42;
        a = find_or_create_node (f, USFS_ROOT_ID, "a", &st);
        alias = find_or_create_node (f, USFS_ROOT_ID, "b", &st);
        st.st_ino = 43;
        other = find_or_create_node (f, USFS_ROOT_ID, "independent", &st);
        if (a == NULL || alias != a || other == NULL)
            abort ();
        memset (&append_test, 0, sizeof (append_test));
        append_test.active = 1;
        first = (struct append_job){ f, a->id, 1, 0 };
        second = (struct append_job){ f, scenario == 3 ? other->id : alias->id, scenario == 3 ? 3 : 2, (int)scenario };
        if (pthread_create (&one, NULL, append_dispatch, &first) != 0)
            abort ();
        pthread_mutex_lock (&ns_test_lock);
        ns_wait_for (&append_test.entered, 1);
        if (pthread_create (&two, NULL, append_dispatch, &second) != 0)
            abort ();
        ns_wait_for (scenario == 3 ? &append_test.second_mutations : &append_test.events, scenario == 3 ? 1 : 2);
        valid = scenario == 3 ? append_test.second_mutations == 1 : append_test.second_mutations == 0;
        append_test.release = 1;
        pthread_cond_broadcast (&ns_test_changed);
        pthread_mutex_unlock (&ns_test_lock);
        pthread_join (one, NULL);
        pthread_join (two, NULL);
        valid &= append_test.bad_handles == 0 && append_test.second_mutations == 1;
        valid &= scenario == 0   ? append_test.size[0] == 2 && memcmp (append_test.data[0], "AB", 2) == 0
                 : scenario == 1 ? append_test.size[0] == 1 && append_test.data[0][0] == 'B'
                 : scenario == 2 ? append_test.size[0] == 0
                                 : append_test.size[0] == 1 && append_test.size[1] == 1;
        tap_ok (
            tap,
            valid,
            scenario == 0   ? "append placement serializes across hard-link aliases"
            : scenario == 1 ? "ordinary writes cannot pass an append's EOF selection"
            : scenario == 2 ? "truncate cannot pass an append's EOF selection"
                            : "independent objects retain concurrent write callbacks"
        );
        append_test.active = 0;
        (last_native_result = usfs_client_destroy (&f));
    }
}

static int removal_error;
static int identity_remove (const struct usfs_client_request * callback_request, const char * path)
{
    (void)callback_request;
    return strcmp (path, "/entry") == 0 ? -removal_error : -EINVAL;
}

static void test_detached_identity (struct tap_state * tap)
{
    unsigned directory;
    for (directory = 0; directory < 2; ++directory)
    {
        struct usfs_operations ops = { 0 };
        struct usfs_client * f;
        struct client_node *old, *replacement;
        struct usfs_in_hdr request = { 0 };
        struct request_context context = { 0 };

        context.header = &request;

        int valid;
        ops.unlink = ops.rmdir = identity_remove;
        f = new_test_client (&ops, sizeof (ops), NULL);
        if (f == NULL)
            abort ();
        f->fd = 73;
        context.client = f;
        request.nodeid = USFS_ROOT_ID;
        old = find_or_create_node (f, USFS_ROOT_ID, "entry", NULL);
        if (old == NULL)
            abort ();
        removal_error = EBUSY;
        handle_unlink_request (&context, "entry", 6);
        valid = captured_reply.error == EBUSY && find_child_node (f, USFS_ROOT_ID, "entry") == old;
        removal_error = 0;
        if (directory)
            handle_rmdir_request (&context, "entry", 6);
        else
            handle_unlink_request (&context, "entry", 6);
        valid &= captured_reply.error == 0 && find_child_node (f, USFS_ROOT_ID, "entry") == NULL && find_node_by_id (f, old->id) == old;
        replacement = find_or_create_node (f, USFS_ROOT_ID, "entry", NULL);
        valid &= replacement != NULL && replacement != old && replacement->id != old->id && find_node_by_id (f, old->id) == old &&
                 find_or_create_node (f, USFS_ROOT_ID, "entry", NULL) == replacement;
        tap_ok (
            tap,
            valid,
            directory ? "rmdir detaches its name while retaining the old identity across replacement"
                      : "unlink detaches only after success and recreation receives a fresh identity"
        );
        (last_native_result = usfs_client_destroy (&f));
    }
}

static unsigned metadata_calls;
static int metadata_chmod (const struct usfs_client_request * callback_request, const char * path, mode_t mode, struct usfs_open_file * fi)
{
    (void)callback_request;
    ++metadata_calls;
    return path == NULL && mode == 0600 && fi != NULL && fi->value == 902 ? 0 : -EIO;
}

static void test_detached_metadata (struct tap_state * tap)
{
    struct usfs_operations ops = { 0 };
    struct usfs_in_hdr in = { 0 };
    struct request_context context = { 0 };

    context.header = &in;

    struct usfs_setattr_in change = { 0 };
    struct usfs_getattr_in attributes = { 0 };
    struct usfs_client * f;
    struct client_node * old;
    ops.unlink = identity_remove;
    ops.chmod = metadata_chmod;
    f = new_test_client (&ops, sizeof (ops), NULL);
    if (f == NULL)
        abort ();
    f->fd = 73;
    context.client = f;
    old = find_or_create_node (f, USFS_ROOT_ID, "entry", NULL);
    if (old == NULL)
        abort ();
    in.nodeid = USFS_ROOT_ID;
    removal_error = 0;
    handle_unlink_request (&context, "entry", 6);
    if (find_or_create_node (f, USFS_ROOT_ID, "entry", NULL) == NULL)
        abort ();
    in.nodeid = old->id;
    change.valid = USFS_SET_MODE;
    change.mode = 0600;
    metadata_calls = 0;
    handle_setattr_request (&context, (char *)&change, sizeof (change));
    tap_ok (tap, captured_reply.error == ESTALE && metadata_calls == 0, "detached setattr without a handle cannot mutate a replacement pathname");
    change.fh = 902;
    handle_setattr_request (&context, (char *)&change, sizeof (change));
    tap_ok (tap, captured_reply.error == 0 && metadata_calls == 1, "detached setattr supplies the retained handle and no former pathname");
    change.valid |= USFS_SET_CTIME;
    handle_setattr_request (&context, (char *)&change, sizeof (change));
    tap_ok (tap, captured_reply.error == EOPNOTSUPP && metadata_calls == 1, "unsupported ctime rejects a mixed request before chmod");
    change.valid = USFS_SET_CTIME;
    handle_setattr_request (&context, (char *)&change, sizeof (change));
    tap_ok (tap, captured_reply.error == EOPNOTSUPP && metadata_calls == 1, "ctime-only request cannot silently succeed");
    handle_getattr_request (&context, (char *)&attributes, sizeof (attributes));
    tap_ok (tap, captured_reply.error == ESTALE, "detached metadata authorization without a handle fails closed");
    (last_native_result = usfs_client_destroy (&f));
}

static int parent_getattr (const struct usfs_client_request * callback_request, const char * path, struct stat * st, struct usfs_open_file * fi)
{
    (void)callback_request;
    (void)fi;
    memset (st, 0, sizeof (*st));
    st->st_dev = 1;
    st->st_nlink = 2;
    st->st_mode = S_IFDIR | 0755;
    st->st_ino = strcmp (path, "/") == 0 ? 1 : strcmp (path, "/a") == 0 ? 2 : strcmp (path, "/b") == 0 ? 3 : 4;
    return 0;
}

static int parent_rename (const struct usfs_client_request * callback_request, const char * from, const char * to)
{
    (void)callback_request;
    return strcmp (from, "/a/child") == 0 && strcmp (to, "/b/child") == 0 ? 0 : -EINVAL;
}

static void test_parent_identity (struct tap_state * tap)
{
    struct usfs_operations ops = { 0 };
    struct usfs_client * f;
    struct client_node *a, *b, *child;
    struct usfs_in_hdr in = { 0 };
    struct request_context context = { 0 };

    context.header = &in;

    struct usfs_rename_in rename = { 0 };
    char payload[sizeof (rename) + 12];
    uint64_t next;
    unsigned i;
    int valid = 1;
    ops.getattr = parent_getattr;
    ops.rename = parent_rename;
    f = new_test_client (&ops, sizeof (ops), NULL);
    if (f == NULL)
        abort ();
    f->fd = 73;
    context.client = f;
    a = find_or_create_node (f, USFS_ROOT_ID, "a", NULL);
    b = find_or_create_node (f, USFS_ROOT_ID, "b", NULL);
    child = a == NULL ? NULL : find_or_create_node (f, a->id, "child", NULL);
    if (b == NULL || child == NULL)
        abort ();
    next = f->next_id;
    in.nodeid = child->id;
    handle_lookup_request (&context, "..", 3);
    tap_ok (tap, captured_reply.error == 0 && captured_entry.nodeid == a->id, "dot-dot returns the existing canonical parent identity");
    in.nodeid = a->id;
    rename.newparent = b->id;
    rename.oldnamelen = 6;
    memcpy (payload, &rename, sizeof (rename));
    memcpy (payload + sizeof (rename), "child\0child", 12);
    handle_rename_request (&context, payload, sizeof (payload));
    in.nodeid = child->id;
    handle_lookup_request (&context, "..", 3);
    tap_ok (tap, captured_reply.error == 0 && captured_entry.nodeid == b->id, "dot-dot follows the published cross-parent directory rename");
    for (i = 0; i < 20; ++i)
    {
        in.nodeid = child->id;
        handle_lookup_request (&context, ".", 2);
        valid &= captured_reply.error == 0 && captured_entry.nodeid == child->id;
        handle_lookup_request (&context, "..", 3);
        valid &= captured_reply.error == 0 && captured_entry.nodeid == b->id;
        in.nodeid = b->id;
        handle_lookup_request (&context, "..", 3);
        valid &= captured_reply.error == 0 && captured_entry.nodeid == USFS_ROOT_ID;
        in.nodeid = USFS_ROOT_ID;
        handle_lookup_request (&context, "..", 3);
        valid &= captured_reply.error == 0 && captured_entry.nodeid == USFS_ROOT_ID;
    }
    tap_ok (tap, valid && f->next_id == next, "repeated dot and ancestor traversal allocate no duplicate identities");
    (last_native_result = usfs_client_destroy (&f));
}

static unsigned hardlink_names, hardlink_calls;

static int hardlink_getattr (const struct usfs_client_request * callback_request, const char * path, struct stat * st, struct usfs_open_file * fi)
{
    (void)callback_request;
    unsigned bit = strcmp (path, "/a") == 0 ? 1 : strcmp (path, "/b") == 0 ? 2 : strcmp (path, "/c") == 0 ? 4 : 0;
    (void)fi;
    memset (st, 0, sizeof (*st));
    st->st_mode = S_IFREG | 0644;
    st->st_dev = 7;
    st->st_ino = 42;
    st->st_nlink = 1;
    if (bit != 0)
    {
        if ((hardlink_names & bit) == 0)
            return -ENOENT;
        st->st_nlink = ((hardlink_names & 1) != 0) + ((hardlink_names & 2) != 0) + ((hardlink_names & 4) != 0);
    }
    else if (strcmp (path, "/other-device") == 0)
    {
        st->st_dev = 8;
    }
    else if (strcmp (path, "/wrong-type") == 0)
    {
        st->st_mode = S_IFLNK | 0777;
    }
    else if (strcmp (path, "/no-inode") == 0)
    {
        st->st_ino = 0;
        st->st_nlink = 2;
    }
    else if (strcmp (path, "/bad-time") == 0)
    {
        st->st_atim.tv_nsec = 1000000000;
    }
    else if (strcmp (path, "/reused") != 0)
    {
        return -ENOENT;
    }
    return 0;
}

static int hardlink_link (const struct usfs_client_request * callback_request, const char * from, const char * to)
{
    (void)callback_request;
    (void)from;
    ++hardlink_calls;
    if (strcmp (to, "/c") != 0)
        return -EINVAL;
    hardlink_names |= 4;
    return 0;
}

static int hardlink_unlink (const struct usfs_client_request * callback_request, const char * path)
{
    (void)callback_request;
    unsigned bit = strcmp (path, "/a") == 0 ? 1 : strcmp (path, "/b") == 0 ? 2 : 4;
    if ((hardlink_names & bit) == 0)
        return -ENOENT;
    hardlink_names &= ~bit;
    return 0;
}

static int hardlink_rename (const struct usfs_client_request * callback_request, const char * from, const char * to)
{
    (void)callback_request;
    return strcmp (from, "/a") == 0 && strcmp (to, "/b") == 0 ? 0 : -EINVAL;
}

static uint64_t hardlink_lookup (struct usfs_client * f, const char * name)
{
    struct usfs_in_hdr in = { 0 };
    struct request_context context = { 0 };

    context.header = &in;
    context.client = f;

    in.nodeid = USFS_ROOT_ID;
    memset (&captured_entry, 0, sizeof (captured_entry));
    handle_lookup_request (&context, name, (uint32_t)strlen (name) + 1);
    return captured_reply.error == 0 ? captured_entry.nodeid : 0;
}

static void test_hardlink_identity (struct tap_state * tap)
{
    struct usfs_operations ops = { 0 };
    struct usfs_client * f;
    struct usfs_in_hdr in = { 0 };
    struct request_context context = { 0 };

    context.header = &in;

    struct usfs_link_in link = { 0 };
    struct usfs_rename_in rename = { 0 };
    char payload[sizeof (rename) + 4], path[64];
    uint64_t first, second, reused;
    struct client_node *a, *b, *c;
    unsigned before;
    ops.getattr = hardlink_getattr;
    ops.link = hardlink_link;
    ops.unlink = hardlink_unlink;
    ops.rename = hardlink_rename;
    f = new_test_client (&ops, sizeof (ops), NULL);
    if (f == NULL)
        abort ();
    f->fd = 73;
    context.client = f;
    hardlink_names = 3;
    hardlink_calls = 0;
    first = hardlink_lookup (f, "a");
    second = hardlink_lookup (f, "b");
    tap_ok (tap, first != 0 && first == second, "pre-existing hard links resolve to one canonical object ID");
    link.newparent = USFS_ROOT_ID;
    memcpy (payload, &link, sizeof (link));
    memcpy (payload + sizeof (link), "c", 2);
    in.nodeid = first;
    before = hardlink_calls;
    fail_mount_calloc = 1;
    handle_link_request (&context, payload, sizeof (link) + 2);
    fail_mount_calloc = 0;
    tap_ok (tap, captured_reply.error == ENOMEM && hardlink_calls == before, "LINK reserves alias storage before its backend mutation");
    hardlink_names &= ~4u;
    handle_link_request (&context, payload, sizeof (link) + 2);
    c = find_child_node (f, USFS_ROOT_ID, "c");
    tap_ok (
        tap,
        captured_reply.error == 0 && c != NULL && c->id == first && hardlink_lookup (f, "c") == first,
        "LINK publishes an alias of the existing object before replying"
    );
    rename.newparent = USFS_ROOT_ID;
    rename.oldnamelen = 2;
    memcpy (payload, &rename, sizeof (rename));
    memcpy (payload + sizeof (rename), "a\0b", 4);
    in.nodeid = USFS_ROOT_ID;
    handle_rename_request (&context, payload, sizeof (rename) + 4);
    a = find_child_node (f, USFS_ROOT_ID, "a");
    b = find_child_node (f, USFS_ROOT_ID, "b");
    tap_ok (tap, captured_reply.error == 0 && a != NULL && a == b, "rename between aliases of the same object preserves both names");
    handle_unlink_request (&context, "a", 2);
    tap_ok (
        tap,
        captured_reply.error == 0 && hardlink_lookup (f, "b") == first && build_node_path (f, first, path, sizeof (path)) == 0 &&
            strcmp (path, "/a") != 0,
        "removing one alias preserves identity and selects a remaining path"
    );
    handle_unlink_request (&context, "b", 2);
    handle_unlink_request (&context, "c", 2);
    reused = hardlink_lookup (f, "reused");
    tap_ok (
        tap,
        reused != 0 && reused != first && find_node_by_id (f, first) != NULL,
        "inode-number reuse after the last unlink cannot revive a retained generation"
    );
    second = hardlink_lookup (f, "other-device");
    tap_ok (tap, second != 0 && second != reused, "equal inode numbers on different backend devices remain separate objects");
    second = hardlink_lookup (f, "wrong-type");
    tap_ok (tap, second == 0 && captured_reply.error == EIO, "conflicting types for one live backend identity fail closed");
    second = hardlink_lookup (f, "no-inode");
    tap_ok (tap, second == 0 && captured_reply.error == EIO, "pre-existing hard links without a stable inode identity are rejected");
    second = hardlink_lookup (f, "bad-time");
    tap_ok (tap, second == 0 && captured_reply.error == EIO, "malformed backend attributes cannot publish or reconcile an identity");
    (last_native_result = usfs_client_destroy (&f));

    f = new_test_client (&ops, sizeof (ops), NULL);
    if (f == NULL)
        abort ();
    f->fd = 73;
    context.client = f;
    hardlink_names = 3;
    first = hardlink_lookup (f, "a");
    handle_unlink_request (&context, "a", 2);
    second = hardlink_lookup (f, "b");
    tap_ok (tap, first != 0 && first == second, "an unobserved surviving hard link retains the object's identity generation");
    (last_native_result = usfs_client_destroy (&f));
}

/* Exercise the real example backend in the existing allocation suite. These
 * interpositions affect only memfs; no fault controls enter the release API. */
static void * memfs_allocations[64];
static size_t memfs_allocation_sizes[64];
static int memfs_buffer_allocations[64];
static unsigned memfs_live, memfs_calls, memfs_fail_at;

static int memfs_allocation_fails (void)
{
    ++memfs_calls;
    if (memfs_fail_at != 0 && memfs_calls == memfs_fail_at)
    {
        errno = ENOMEM;
        return 1;
    }
    return 0;
}
static void * memfs_track (void * pointer, size_t size, int buffer)
{
    unsigned i;
    if (pointer == NULL)
        return NULL;
    for (i = 0; i < 64; ++i)
        if (memfs_allocations[i] == NULL)
        {
            memfs_allocations[i] = pointer;
            memfs_allocation_sizes[i] = size;
            memfs_buffer_allocations[i] = buffer;
            ++memfs_live;
            return pointer;
        }
    abort ();
}
static void * memfs_test_calloc (size_t count, size_t size)
{
    return memfs_allocation_fails () ? NULL : memfs_track (calloc (count, size), count * size, 0);
}
static char * memfs_test_strdup (const char * text)
{
    return memfs_allocation_fails () ? NULL : memfs_track (strdup (text), strlen (text) + 1, 0);
}
static void memfs_test_free (void * pointer)
{
    unsigned i;
    if (pointer == NULL)
        return;
    for (i = 0; i < 64; ++i)
        if (memfs_allocations[i] == pointer)
        {
            memfs_allocations[i] = NULL;
            memfs_allocation_sizes[i] = 0;
            memfs_buffer_allocations[i] = 0;
            --memfs_live;
            free (pointer);
            return;
        }
    abort ();
}
static void * memfs_test_realloc (void * pointer, size_t size)
{
    unsigned i = 0;
    const int was_null = pointer == NULL;
    void * result;
    if (memfs_allocation_fails ())
        return NULL;
    if (!was_null)
    {
        for (; i < 64 && memfs_allocations[i] != pointer; ++i)
        {
        }
        if (i == 64)
            abort ();
    }
    result = realloc (pointer, size);
    if (result == NULL)
        return NULL;
    if (was_null)
        return memfs_track (result, size, 1);
    memfs_allocations[i] = result;
    memfs_allocation_sizes[i] = size;
    return result;
}

static size_t memfs_buffer_bytes (void)
{
    size_t bytes = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (memfs_buffer_allocations[i])
            bytes += memfs_allocation_sizes[i];
    return bytes;
}

#define calloc  memfs_test_calloc
#define strdup  memfs_test_strdup
#define realloc memfs_test_realloc
#define free    memfs_test_free
#define main    memfs_example_main
#include "../../../examples/memfs.c"
#undef main
#undef free
#undef realloc
#undef strdup
#undef calloc

static void memfs_fixture (void)
{
    if (memfs_live != 0 || g_inodes != NULL)
        abort ();
    memfs_fail_at = memfs_calls = 0;
    g_next_ino = g_next_handle = 1;
    g_handles = NULL;
    g_inode_count = g_used = g_allocated = 0;
    g_root = inode_new (S_IFDIR | 0755, 0, 0);
    if (g_root == NULL)
        abort ();
    g_root->nlink = 2;
}
static int memfs_fixture_clean (void)
{
    memfs_fail_at = 0;
    while (g_handles != NULL)
        handle_close (g_handles->id);
    while (g_root->entries != NULL)
    {
        struct mem_inode * inode = g_root->entries->inode;
        dir_remove (g_root, g_root->entries);
        inode_maybe_free (inode);
    }
    g_root->nlink = 0;
    inode_maybe_free (g_root);
    g_root = NULL;
    return memfs_live == 0 && g_inodes == NULL && g_inode_count == 0 && g_used == 0 && g_allocated == 0 && memfs_buffer_bytes () == 0;
}

static void test_memfs_capacity (struct tap_state * tap)
{
    struct usfs_open_file a = { 0 }, b = { 0 };
    struct statvfs st;
    char byte = 0;
    const size_t saved_max = g_max;
    memfs_fixture ();
    g_max = 8192;
    a.open_flags = b.open_flags = O_RDWR;
    int valid = memfs_create (get_callback_request (), "/a", 0600, &a) == 0 && memfs_create (get_callback_request (), "/b", 0600, &b) == 0 &&
                memfs_truncate (get_callback_request (), "/a", 8192, &a) == 0 && memfs_truncate (get_callback_request (), "/a", 0, &a) == 0 &&
                memfs_truncate (get_callback_request (), "/b", 8192, &b) == 0 && memfs_write (get_callback_request (), "/b", "b", 1, 0, &b) == 1;
    valid &= memfs_truncate (get_callback_request (), "/a", 8192, &a) == -ENOSPC && g_used == g_max && resolve ("/a", NULL, NULL)->size == 0 &&
             memfs_statfs (get_callback_request (), "/", &st) == 0 && st.f_bfree == 0 &&
             memfs_read (get_callback_request (), "/b", &byte, 1, 0, &b) == 1 && byte == 'b';
    valid &= g_allocated == 8192 && memfs_buffer_bytes () == 8192;
    tap_ok (tap, valid, "truncate releases buffers before capacity is reused by another file");
    valid = memfs_truncate (get_callback_request (), "/b", 0, &b) == 0 && memfs_truncate (get_callback_request (), "/a", 8192, &a) == 0 &&
            g_used == g_max && memfs_read (get_callback_request (), "/a", &byte, 1, 0, &a) == 1 && byte == 0;
    tap_ok (tap, valid, "regrowth succeeds with zero-filled bytes after capacity is freed");
    ++g_used;
    valid = memfs_statfs (get_callback_request (), "/", &st) == -EIO && memfs_truncate (get_callback_request (), "/b", 1, &b) == -EIO;
    --g_used;
    valid &= memfs_fixture_clean ();
    g_max = saved_max;
    tap_ok (tap, valid, "invalid capacity accounting reports EIO without unsigned wraparound");
}

static void test_memfs_buffer_capacity (struct tap_state * tap)
{
    const size_t saved_max = g_max;
    struct usfs_open_file a = { 0 }, b = { 0 };
    struct statvfs st;
    char name[32], bytes[8192], readback[16];
    int valid = 1;
    memfs_fixture ();
    g_max = 8192;
    memset (bytes, 'x', sizeof (bytes));
    for (unsigned i = 0; i < 8; ++i)
    {
        struct usfs_open_file fi = { 0 };
        fi.open_flags = O_RDWR;
        snprintf (name, sizeof (name), "/empty%u", i);
        valid &= memfs_create (get_callback_request (), name, 0600, &fi) == 0 && memfs_buffer_bytes () == 0;
        valid &= memfs_write (get_callback_request (), name, bytes, sizeof (bytes), 0, &fi) == sizeof (bytes) && memfs_buffer_bytes () == 8192 &&
                 g_allocated == 8192;
        valid &= memfs_truncate (get_callback_request (), name, 0, &fi) == 0 && memfs_release (get_callback_request (), name, &fi) == 0 &&
                 memfs_buffer_bytes () == 0 && g_allocated == 0 && resolve (name, NULL, NULL)->size == 0;
    }
    valid &= memfs_statfs (get_callback_request (), "/", &st) == 0 && st.f_bfree == 2 && memfs_fixture_clean ();
    tap_ok (tap, valid, "grow-shrink across retained empty names never accumulates file buffers");

    memfs_fixture ();
    g_max = 16384;
    a.open_flags = b.open_flags = O_RDWR;
    valid = memfs_create (get_callback_request (), "/a", 0600, &a) == 0 && memfs_create (get_callback_request (), "/b", 0600, &b) == 0 &&
            memfs_write (get_callback_request (), "/a", bytes, sizeof (bytes), 0, &a) == sizeof (bytes);
    memfs_calls = 0;
    memfs_fail_at = 1;
    valid &= memfs_truncate (get_callback_request (), "/a", 100, &a) == 0 && memfs_calls == 1 && memfs_buffer_bytes () == 8192 &&
             g_allocated == 8192 && g_used == 100 && memfs_statfs (get_callback_request (), "/", &st) == 0 && st.f_bfree == 2;
    memfs_fail_at = 0;
    valid &= memfs_write (get_callback_request (), "/b", bytes, sizeof (bytes), 0, &b) == sizeof (bytes) &&
             memfs_truncate (get_callback_request (), "/a", 8192, &a) == 0 && memfs_buffer_bytes () == 16384 &&
             memfs_read (get_callback_request (), "/a", readback, sizeof (readback), 100, &a) == sizeof (readback);
    for (unsigned i = 0; i < sizeof (readback); ++i)
        valid &= readback[i] == 0;
    valid &= memfs_truncate (get_callback_request (), "/a", 4096, &a) == 0 && memfs_buffer_bytes () == 12288 && g_allocated == 12288 &&
             memfs_statfs (get_callback_request (), "/", &st) == 0 && st.f_bfree == 1;
    valid &= memfs_fixture_clean ();
    tap_ok (tap, valid, "failed shrink retains its charge and zero-filled regrowth; successful shrink refunds bytes");

    memfs_fixture ();
    g_max = 5000;
    a.open_flags = b.open_flags = O_RDWR;
    valid = memfs_create (get_callback_request (), "/a", 0600, &a) == 0 && memfs_create (get_callback_request (), "/b", 0600, &b) == 0 &&
            memfs_write (get_callback_request (), "/a", bytes, 1, 0, &a) == 1 && memfs_buffer_bytes () == 4096 &&
            memfs_write (get_callback_request (), "/b", bytes, 1000, 0, &b) == 904 && memfs_buffer_bytes () == 5000 && g_allocated == 5000 &&
            memfs_statfs (get_callback_request (), "/", &st) == 0 && st.f_bfree == 0 &&
            memfs_write (get_callback_request (), "/a", bytes, 4096, 0, &a) == 4096 &&
            memfs_write (get_callback_request (), "/b", "z", 1, 0, &b) == 1 &&
            memfs_write (get_callback_request (), "/b", bytes, 1, 904, &b) == -ENOSPC &&
            memfs_write (get_callback_request (), "/b", bytes, 0, 9000, &b) == 0 && memfs_buffer_bytes () == 5000;
    valid &= memfs_fixture_clean ();
    tap_ok (tap, valid, "allocation budget clips geometric growth and permits spare-capacity writes, overwrites and empty writes");

    memfs_fixture ();
    g_max = 8192;
    a.open_flags = O_RDWR;
    valid = memfs_create (get_callback_request (), "/a", 0600, &a) == 0 && memfs_write (get_callback_request (), "/a", "abc", 3, 0, &a) == 3;
    memfs_calls = 0;
    memfs_fail_at = 1;
    valid &= memfs_write (get_callback_request (), "/a", bytes, 8192, 0, &a) == -ENOMEM && memfs_buffer_bytes () == 4096 && g_allocated == 4096 &&
             g_used == 3 && memfs_read (get_callback_request (), "/a", readback, 3, 0, &a) == 3 && memcmp (readback, "abc", 3) == 0;
    memfs_fail_at = 0;
    valid &= memfs_link (get_callback_request (), "/a", "/alias") == 0 && memfs_buffer_bytes () == 4096 &&
             memfs_unlink (get_callback_request (), "/a") == 0 && memfs_unlink (get_callback_request (), "/alias") == 0 &&
             memfs_buffer_bytes () == 4096 && memfs_read (get_callback_request (), NULL, readback, 3, 0, &a) == 3 &&
             memfs_release (get_callback_request (), NULL, &a) == 0 && memfs_buffer_bytes () == 0 && g_allocated == 0;
    valid &= memfs_fixture_clean ();
    g_max = saved_max;
    tap_ok (tap, valid, "failed growth preserves data and accounting; aliases and unlinked handles retain one charged buffer");
}

static void test_memfs_reservations (struct tap_state * tap)
{
    unsigned directory, fault;
    {
        struct usfs_client_request saved = *get_callback_request ();
        struct usfs_open_file fi = { 0 };
        struct mem_inode *dir, *file, *linknode;
        int valid;
        memfs_fixture ();
        get_callback_request ()->uid = 1001;
        get_callback_request ()->gid = 2001;
        g_root->mode |= S_ISGID;
        g_root->gid = 3001;
        valid = memfs_mkdir (get_callback_request (), "/private", 0700) == 0 && memfs_create (get_callback_request (), "/file", 0600, &fi) == 0 &&
                memfs_symlink (get_callback_request (), "file", "/link") == 0;
        dir = resolve ("/private", NULL, NULL);
        file = resolve ("/file", NULL, NULL);
        linknode = resolve ("/link", NULL, NULL);
        valid &= dir && file && linknode && dir->uid == 1001 && file->uid == 1001 && linknode->uid == 1001 && dir->gid == 3001 && file->gid == 3001 &&
                 linknode->gid == 3001 && (dir->mode & 07777) == 02700;
        *get_callback_request () = saved;
        valid &= memfs_fixture_clean ();
        tap_ok (tap, valid, "creation uses request ownership and native AIX setgid inheritance");
    }
    for (directory = 0; directory < 2; ++directory)
        for (fault = 1; fault <= 2; ++fault)
        {
            struct usfs_open_file source = { 0 }, target = { 0 };
            struct mem_inode *src, *dst;
            unsigned before;
            int valid;
            char data[6];
            memfs_fixture ();
            source.open_flags = target.open_flags = O_RDWR;
            if (directory)
            {
                if (memfs_mkdir (get_callback_request (), "/source", 0700) || memfs_mkdir (get_callback_request (), "/target", 0750) ||
                    memfs_opendir (get_callback_request (), "/source", &source) || memfs_opendir (get_callback_request (), "/target", &target))
                    abort ();
            }
            else if (memfs_create (get_callback_request (), "/source", 0640, &source) || memfs_create (get_callback_request (), "/target", 0604, &target) || memfs_write (get_callback_request (), "/source", "source", 6, 0, &source) != 6 || memfs_write (get_callback_request (), "/target", "target", 6, 0, &target) != 6 || memfs_link (get_callback_request (), "/source", "/alias"))
                abort ();
            src = resolve ("/source", NULL, NULL);
            dst = resolve ("/target", NULL, NULL);
            before = memfs_live;
            g_root->mtime.tv_sec = 123;
            memfs_calls = 0;
            memfs_fail_at = fault;
            valid = memfs_rename (get_callback_request (), "/source", "/target") == -ENOMEM && memfs_calls == fault && memfs_live == before &&
                    g_inode_count == 3 && resolve ("/source", NULL, NULL) == src && resolve ("/target", NULL, NULL) == dst &&
                    src->nlink == (directory ? 1 : 2) && dst->nlink == 1 && src->open_count == 1 && dst->open_count == 1 &&
                    g_root->mtime.tv_sec == 123;
            memfs_fail_at = 0;
            valid &= memfs_rename (get_callback_request (), "/source", "/target") == 0 && resolve ("/source", NULL, NULL) == NULL &&
                     resolve ("/target", NULL, NULL) == src && src->nlink == (directory ? 1 : 2) && dst->nlink == 0 && src->open_count == 1 &&
                     dst->open_count == 1;
            if (!directory)
                valid &= g_used == 12 && resolve ("/alias", NULL, NULL) == src &&
                         memfs_read (get_callback_request (), NULL, data, 6, 0, &source) == 6 && memcmp (data, "source", 6) == 0 &&
                         memfs_read (get_callback_request (), NULL, data, 6, 0, &target) == 6 && memcmp (data, "target", 6) == 0;
            else
                valid &= src->parent == g_root && dst->parent == NULL;
            valid &= memfs_fixture_clean ();
            tap_ok (
                tap,
                valid,
                directory ? "directory rename allocation failure preserves both objects and retry ownership"
                          : "file rename allocation failure preserves names, contents, links and handles through retry"
            );
        }
    for (fault = 1; fault <= 4; ++fault)
    {
        struct usfs_open_file fi = { 0 };
        struct mem_inode * inode;
        int valid;
        char data[2];
        memfs_fixture ();
        fi.open_flags = O_RDWR;
        g_root->mtime.tv_sec = 123;
        memfs_calls = 0;
        memfs_fail_at = fault;
        valid = memfs_create (get_callback_request (), "/created", 0640, &fi) == -ENOMEM && memfs_calls == fault && fi.value == 0 &&
                g_handles == NULL && resolve ("/created", NULL, NULL) == NULL && g_inode_count == 1 && g_used == 0 && memfs_live == 1 &&
                g_root->mtime.tv_sec == 123;
        memfs_fail_at = 0;
        valid &= memfs_create (get_callback_request (), "/created", 0640, &fi) == 0 && fi.value != 0;
        inode = resolve ("/created", NULL, NULL);
        valid &= inode != NULL && inode->nlink == 1 && inode->open_count == 1 && (inode->mode & 0777) == 0640 && g_inode_count == 2 &&
                 memfs_write (get_callback_request (), "/created", "ok", 2, 0, &fi) == 2 &&
                 memfs_read (get_callback_request (), NULL, data, 2, 0, &fi) == 2 && memcmp (data, "ok", 2) == 0;
        valid &= memfs_fixture_clean ();
        tap_ok (tap, valid, "CREATE reserves every allocation before publication and retries without leaked ownership");
    }
}

static void test_atomic_create (struct tap_state * tap)
{
    unsigned activation, failure;
    for (activation = 0; activation <= 2; ++activation)
    {
        unsigned count = activation == USFS_CREATE_OPEN ? 5 : 4;
        for (failure = 0; failure <= count; ++failure)
        {
            struct usfs_operations ops = { 0 };
            struct usfs_create_attr_in cin = { 0 };
            struct usfs_in_hdr in = { 0 };
            struct request_context context = { 0 };

            context.header = &in;

            char payload[sizeof (cin) + 4];
            struct usfs_client * f;
            struct mem_inode * created;
            unsigned baseline;
            int valid = 1;
            memfs_fixture ();
            ops.create_attr = memfs_create_attr;
            f = new_test_client (&ops, sizeof (ops), NULL);
            if (f == NULL)
                abort ();
            f->fd = 73;
            context.client = f;
            in.nodeid = 1;
            cin.flags = activation == USFS_CREATE_OPEN ? FREAD | FWRITE : 0;
            cin.activation = activation;
            cin.attr.valid = USFS_SET_MODE | USFS_SET_UID | USFS_SET_GID | USFS_SET_SIZE | USFS_SET_ATIME | USFS_SET_MTIME | USFS_SET_CTIME;
            cin.attr.mode = 0600;
            cin.attr.uid = 1001;
            cin.attr.gid = 1002;
            cin.attr.size = 17;
            cin.attr.atime = 11;
            cin.attr.atimensec = 12;
            cin.attr.mtime = 21;
            cin.attr.mtimensec = 22;
            cin.attr.ctime = 31;
            cin.attr.ctimensec = 32;
            memcpy (payload, &cin, sizeof (cin));
            memcpy (payload + sizeof (cin), "new", 4);
            baseline = memfs_live;
            memfs_calls = 0;
            memfs_fail_at = failure;
            handle_create_attr_request (&context, payload, sizeof (payload));
            if (failure != 0)
            {
                valid = captured_reply.error == ENOMEM && resolve ("/new", NULL, NULL) == NULL && memfs_live == baseline && g_handles == NULL &&
                        g_inode_count == 1 && g_used == 0 && g_allocated == 0 && memfs_buffer_bytes () == 0;
                memfs_fail_at = 0;
                handle_create_attr_request (&context, payload, sizeof (payload));
            }
            created = resolve ("/new", NULL, NULL);
            valid &= captured_reply.error == 0 && created != NULL && usfs_create_attr_out_valid (&captured_create, activation) &&
                     captured_create.attr.uid == 1001 && captured_create.attr.gid == 1002 && captured_create.attr.mode == (S_IFREG | 0600) &&
                     captured_create.attr.size == 17 && captured_create.attr.atimensec == 12 && captured_create.attr.mtimensec == 22 &&
                     captured_create.attr.ctimensec == 32 && (g_handles != NULL) == (activation == USFS_CREATE_OPEN);
            if (created != NULL)
            {
                unsigned i;
                valid &= created->open_count == (activation == USFS_CREATE_OPEN) && created->nlink == 1;
                for (i = 0; i < 17; ++i)
                    valid &= created->data[i] == 0;
            }
            (last_native_result = usfs_client_destroy (&f));
            valid &= memfs_fixture_clean ();
            tap_ok (tap, valid, "atomic CREATE_ATTR reserves storage before publication, honors activation and retries every allocation failure");
        }
    }
    for (failure = 0; failure < 6; ++failure)
    {
        struct usfs_operations ops = { 0 };
        struct usfs_create_attr_in cin = { 0 };
        struct usfs_in_hdr in = { 0 };
        struct request_context context = { 0 };

        context.header = &in;

        char payload[sizeof (cin) + 4];
        struct usfs_client * f;
        int expected = EINVAL, valid;
        memfs_fixture ();
        ops.create_attr = failure < 4 ? memfs_create_attr : NULL;
        f = new_test_client (&ops, sizeof (ops), NULL);
        if (f == NULL)
            abort ();
        f->fd = 73;
        context.client = f;
        in.nodeid = 1;
        cin.flags = FREAD | FWRITE;
        cin.activation = USFS_CREATE_OPEN;
        cin.attr.valid = USFS_SET_MODE;
        cin.attr.mode = 0600;
        switch (failure)
        {
            case 0:
                cin.attr.valid |= USFS_SET_TIMES_NOW;
                break;
            case 1:
                cin.attr.valid |= USFS_SET_SIZE;
                cin.attr.size = UINT64_MAX;
                break;
            case 2:
                cin.attr.valid |= USFS_SET_MTIME;
                cin.attr.mtimensec = 1000000000u;
                break;
            case 3:
                fail_mount_calloc = 1;
                expected = ENOMEM;
                break;
            case 4:
                cin.attr.valid |= USFS_SET_UID;
                expected = EOPNOTSUPP;
                break;
            case 5:
                cin.activation = USFS_CREATE_LOOKUP;
                expected = EOPNOTSUPP;
                break;
        }
        memcpy (payload, &cin, sizeof (cin));
        memcpy (payload + sizeof (cin), "new", 4);
        memfs_calls = 0;
        handle_create_attr_request (&context, payload, sizeof (payload));
        fail_mount_calloc = 0;
        valid = captured_reply.error == expected && memfs_calls == 0 && g_handles == NULL && resolve ("/new", NULL, NULL) == NULL;
        (last_native_result = usfs_client_destroy (&f));
        valid &= memfs_fixture_clean ();
        tap_ok (tap, valid, "invalid or unsupported atomic creation and local reservation failure reject before backend mutation");
    }
}

static unsigned directory_callbacks;
static int directory_callback_error;

static void directory_test_handle (struct usfs_client * client, const uint64_t nodeid, const uint64_t backend_handle)
{
    struct client_handle * handle = calloc (1, sizeof (*handle));
    struct open_handle_state opened = { 0 };

    if (handle == NULL)
        abort ();

    opened.file_info.value = backend_handle;
    opened.nodeid = nodeid;
    opened.is_directory = true;
    publish_client_handle (client, handle, &opened);
}

static int directory_cached (struct usfs_client * f, uint64_t nodeid)
{
    for (unsigned i = 0; i < CLIENT_DIRCACHE_SLOTS; ++i)
        if (f->dircache[i].snapshot_id != 0 && f->dircache[i].key.nodeid == nodeid)
            return 1;
    return 0;
}

static int directory_listing (
    const struct usfs_client_request * callback_request,
    const char * path,
    struct usfs_open_file * fi,
    struct usfs_directory_sink * sink
)
{
    (void)callback_request;
    unsigned i;
    (void)path;
    (void)fi;
    ++directory_callbacks;
    for (i = 0; i < 600; ++i)
    {
        char name[32];
        snprintf (name, sizeof (name), "entry-%04u", i);
        if (usfs_directory_add (sink, name, NULL))
            break;
    }
    return -directory_callback_error;
}

static unsigned partial_directory_calls;

static int one_entry_then_error (
    const struct usfs_client_request * callback_request,
    const char * path,
    struct usfs_open_file * file_info,
    struct usfs_directory_sink * sink
)
{
    (void)callback_request;
    (void)path;
    (void)file_info;
    partial_directory_calls++;
    if (usfs_directory_add (sink, "entry", NULL) != 0)
        return -EIO;

    return partial_directory_calls == 1 ? -EIO : 0;
}

static void test_partial_readdir_error (struct tap_state * tap)
{
    struct usfs_operations operations = { 0 };
    struct usfs_in_hdr header = { 0 };
    struct request_context context = { 0 };
    struct usfs_readdir_in request = { 0 };
    struct usfs_readdir_out reply = { 0 };

    operations.readdir = one_entry_then_error;
    struct usfs_client * client = new_test_client (&operations, sizeof (operations), NULL);
    if (client == NULL)
        abort ();

    client->fd = 73;
    header.nodeid = USFS_ROOT_ID;
    context.client = client;
    context.header = &header;
    request.size = USFS_MAX_DATA;
    partial_directory_calls = 0;

    handle_readdir_request (&context, (const char *)&request, sizeof (request));
    int valid = captured_reply.error == EIO && !directory_cached (client, USFS_ROOT_ID);

    handle_readdir_request (&context, (const char *)&request, sizeof (request));
    memcpy (&reply, get_reply_buffer (client) + sizeof (struct usfs_reply_header), sizeof (reply));
    struct usfs_dirent entry;
    memcpy (&entry, get_reply_buffer (client) + sizeof (struct usfs_reply_header) + sizeof (reply), sizeof (entry));
    valid &= captured_reply.error == 0 && reply.count == 1 && reply.snapshot_id != 0 && entry.namelen == strlen ("entry") &&
             partial_directory_calls == 2 && directory_cached (client, USFS_ROOT_ID);

    tap_ok (tap, valid, "readdir error after one entry discards the partial snapshot and retry starts fresh");
    (last_native_result = usfs_client_destroy (&client));
}

static struct usfs_client * listing_client;
static int listing_release;
static int isolated_listing (
    const struct usfs_client_request * callback_request,
    const char * path,
    struct usfs_open_file * fi,
    struct usfs_directory_sink * sink
)
{
    (void)callback_request;
    const char * order = fi->value == 202 ? "cdab" : "abcd";
    (void)path;
    ++directory_callbacks;
    for (unsigned i = 0; i < 4; ++i)
    {
        char name[2] = { order[i], 0 };
        if (get_callback_request ()->uid == 2)
            name[0] -= 'a' - 'A';
        if (usfs_directory_add (sink, name, NULL))
            break;
    }
    if (listing_release)
    {
        struct client_handle * handle = listing_client->handles;
        listing_client->handles = handle->next;
        dispose_client_handle (listing_client, handle);
        listing_release = 0;
    }
    return 0;
}

static int listing_window (struct usfs_client * f, uint64_t fh, unsigned uid, uint64_t cookie, const char * expected, uint64_t * snapshot_id)
{
    struct usfs_in_hdr in = { 0 };
    struct request_context context = { 0 };

    context.header = &in;
    context.client = f;

    struct usfs_readdir_in din = { 0 };
    struct usfs_readdir_out out;
    size_t pos = sizeof (struct usfs_reply_header) + sizeof (out);
    in.nodeid = USFS_ROOT_ID;
    in.uid = uid;
    in.gid = uid + 10;
    in.pid = uid + 100;
    din.fh = fh;
    din.cookie = cookie;
    din.size = 2 * USFS_DIRENT_SIZE (1);
    set_request_context (f, &in);
    handle_readdir_request (&context, (char *)&din, sizeof (din));
    memcpy (&out, get_reply_buffer (f) + sizeof (struct usfs_reply_header), sizeof (out));
    if (snapshot_id != NULL)
        *snapshot_id = out.snapshot_id;
    if (captured_reply.error || out.count != strlen (expected))
        return 0;
    for (unsigned i = 0; i < out.count; ++i)
    {
        struct usfs_dirent entry;
        memcpy (&entry, get_reply_buffer (f) + pos, sizeof (entry));
        if (entry.namelen != 1 || get_reply_buffer (f)[pos + sizeof (entry)] != expected[i])
            return 0;
        pos += entry.reclen;
    }
    return 1;
}

static uint64_t listing_cursor (uint64_t snapshot_id, uint32_t index)
{
    return (snapshot_id << USFS_DIRECTORY_CURSOR_INDEX_BITS) | index;
}

static void test_directory_isolation (struct tap_state * tap)
{
    struct usfs_operations ops = { 0 };
    struct usfs_client_request saved = *get_callback_request ();
    uint64_t first_id = 0, second_id = 0, other_user_id = 0;

    ops.readdir = isolated_listing;
    struct usfs_client * f = new_test_client (&ops, sizeof (ops), NULL);
    if (f == NULL)
        abort ();

    f->fd = 73;
    directory_callbacks = 0;
    directory_test_handle (f, 1, 101);
    directory_test_handle (f, 1, 202);

    int valid = listing_window (f, 101, 1, 0, "ab", &first_id) && listing_window (f, 202, 1, 0, "cd", &second_id) && first_id != second_id &&
                listing_window (f, 101, 1, listing_cursor (first_id, 2), "cd", NULL) &&
                listing_window (f, 202, 1, listing_cursor (second_id, 2), "ab", NULL);
    tap_ok (tap, valid && directory_callbacks == 2, "independent directory opens retain their own cursor and continuation order");

    valid = listing_window (f, 101, 2, 0, "AB", &other_user_id) && other_user_id != first_id &&
            listing_window (f, 101, 1, listing_cursor (first_id, 2), "cd", NULL) &&
            listing_window (f, 101, 2, listing_cursor (other_user_id, 2), "CD", NULL);
    tap_ok (tap, valid && directory_callbacks == 3, "directory cursors cannot cross request credential contexts");

    valid = !listing_window (f, 101, 2, listing_cursor (first_id, 2), "CD", NULL) && captured_reply.error == ESTALE;
    tap_ok (tap, valid, "cursor identity rejects a different credential context");

    uint64_t handleless_id = 0;
    valid = listing_window (f, 0, 1, 0, "ab", &handleless_id) && listing_window (f, 0, 1, listing_cursor (handleless_id, 2), "cd", NULL);
    tap_ok (tap, valid, "handleless directory walk retains its own snapshot");

    valid = listing_window (f, 0, 1, listing_cursor (handleless_id, 4), "", NULL) &&
            listing_window (f, 0, 1, listing_cursor (handleless_id, 4), "", NULL) &&
            !listing_window (f, 0, 1, listing_cursor (handleless_id, 2), "cd", NULL) && captured_reply.error == ESTALE;
    tap_ok (tap, valid, "completed handleless walk repeats EOF and rejects backward seeks");

    uint64_t evicted_id = 0;
    valid = listing_window (f, 101, 1, 0, "ab", &evicted_id);
    for (unsigned i = 0; i < CLIENT_DIRCACHE_SLOTS + 1; ++i)
    {
        directory_test_handle (f, 1, 300 + i);
        valid &= listing_window (f, 300 + i, 1, 0, "ab", NULL);
    }
    valid &= !listing_window (f, 101, 1, listing_cursor (evicted_id, 2), "cd", NULL) && captured_reply.error == ESTALE;
    valid &= listing_window (f, 101, 1, 0, "ab", NULL);
    tap_ok (tap, valid, "evicted cursor fails explicitly and rewind rebuilds the listing");

    directory_test_handle (f, 1, 505);
    listing_client = f;
    listing_release = 1;
    valid = !listing_window (f, 505, 1, 0, "ab", NULL) && captured_reply.error == ESTALE;
    directory_test_handle (f, 1, 505);
    valid &= listing_window (f, 505, 1, 0, "ab", NULL);
    tap_ok (tap, valid, "release during construction prevents publishing a stale handle snapshot");

    valid = listing_window (f, 0, 1, 0, "ab", &handleless_id);
    invalidate_node_directory_cache (f, 1);
    valid &= !directory_cached (f, 1) && !listing_window (f, 0, 1, listing_cursor (handleless_id, 2), "cd", NULL) && captured_reply.error == ESTALE;
    tap_ok (tap, valid, "namespace invalidation expires handleless and open-handle cursors");

    uint64_t pressure_id = 0;
    valid = listing_window (f, 0, 1, 0, "ab", &pressure_id);
    struct client_dircache * pressure_snapshot = NULL;
    for (unsigned slot_index = 0; slot_index < CLIENT_DIRCACHE_SLOTS; ++slot_index)
        if (f->dircache[slot_index].snapshot_id == pressure_id)
            pressure_snapshot = &f->dircache[slot_index];
    if (pressure_snapshot != NULL)
    {
        pressure_snapshot->spool_reserved = CLIENT_DIRSPOOL_MAX;
        f->dircache_spool_bytes = CLIENT_DIRSPOOL_MAX;
    }
    struct usfs_directory_sink pressure_buffer = { 0 };
    pressure_buffer.client = f;
    valid &= pressure_snapshot != NULL && reserve_directory_spool (&pressure_buffer, 1) == 0 &&
             !listing_window (f, 0, 1, listing_cursor (pressure_id, 2), "cd", NULL) && captured_reply.error == ESTALE;
    release_directory_buffer (&pressure_buffer);
    valid &= f->dircache_spool_bytes == 0;
    tap_ok (tap, valid, "storage pressure reclaims a handleless snapshot and expires its cursor");

    (last_native_result = usfs_client_destroy (&f));
    listing_client = NULL;
    *get_callback_request () = saved;
}

static void test_directory_failures (struct tap_state * tap)
{
    unsigned failure;
    {
        struct usfs_operations ops = { 0 };
        struct usfs_in_hdr in = { 0 };
        struct request_context context = { 0 };

        context.header = &in;

        struct usfs_readdir_in din = { 0 };
        struct usfs_readdir_out out;
        struct usfs_client * f;
        ops.readdir = directory_listing;
        f = new_test_client (&ops, sizeof (ops), NULL);
        if (f == NULL)
            abort ();
        f->fd = 73;
        context.client = f;
        in.nodeid = USFS_ROOT_ID;
        din.fh = 101;
        directory_test_handle (f, in.nodeid, din.fh);
        din.size = 128;
        directory_fail_growth = directory_callback_error = 0;
        handle_readdir_request (&context, (char *)&din, sizeof (din));
        memcpy (&out, get_reply_buffer (f) + sizeof (struct usfs_reply_header), sizeof (out));
        din.cookie = listing_cursor (out.snapshot_id, 1);
        din.size = 1;
        handle_readdir_request (&context, (char *)&din, sizeof (din));
        tap_ok (tap, captured_reply.error == EINVAL, "undersized wire directory buffer reports an error instead of EOF");
        din.size = 128;
        handle_readdir_request (&context, (char *)&din, sizeof (din));
        memcpy (&out, get_reply_buffer (f) + sizeof (struct usfs_reply_header), sizeof (out));
        tap_ok (
            tap,
            captured_reply.error == 0 && out.count > 0 &&
                memcmp (get_reply_buffer (f) + sizeof (struct usfs_reply_header) + sizeof (out) + sizeof (struct usfs_dirent), "entry-0001", 10) == 0,
            "larger directory retry returns the same unconsumed entry"
        );
        (last_native_result = usfs_client_destroy (&f));
    }
    for (failure = 1; failure <= 5; ++failure)
    {
        struct usfs_operations ops = { 0 };
        struct usfs_in_hdr in = { 0 };
        struct request_context context = { 0 };

        context.header = &in;

        struct usfs_readdir_in din = { 0 };
        struct usfs_client * f;
        unsigned seen = 0, replies = 0;
        int valid;
        ops.readdir = directory_listing;
        f = new_test_client (&ops, sizeof (ops), NULL);
        if (f == NULL)
            abort ();
        f->fd = 73;
        context.client = f;
        in.nodeid = USFS_ROOT_ID;
        din.fh = 101;
        directory_test_handle (f, in.nodeid, din.fh);
        din.size = 128;
        directory_callbacks = directory_growths = 0;
        if (failure == 5)
            handle_readdir_request (&context, (char *)&din, sizeof (din));
        directory_fail_growth = failure <= 3 ? failure : failure == 5 ? 2 : 0;
        directory_callback_error = failure == 4 ? EIO : 0;
        handle_readdir_request (&context, (char *)&din, sizeof (din));
        valid = captured_reply.error == (failure == 4 ? EIO : ENOMEM) &&
                (failure == 5 ? directory_cached (f, in.nodeid) : !directory_cached (f, in.nodeid)) && (failure == 5 || directory_allocation == NULL);
        directory_fail_growth = 0;
        directory_callback_error = 0;
        do
        {
            struct usfs_readdir_out out;
            size_t pos = sizeof (struct usfs_reply_header) + sizeof (out);
            unsigned i;
            handle_readdir_request (&context, (char *)&din, sizeof (din));
            if (captured_reply.error != 0)
            {
                valid = 0;
                break;
            }
            memcpy (&out, get_reply_buffer (f) + sizeof (struct usfs_reply_header), sizeof (out));
            for (i = 0; i < out.count; ++i)
            {
                struct usfs_dirent de;
                char expected[32];
                memcpy (&de, get_reply_buffer (f) + pos, sizeof (de));
                snprintf (expected, sizeof (expected), "entry-%04u", seen++);
                valid &= de.namelen == strlen (expected) && memcmp (get_reply_buffer (f) + pos + sizeof (de), expected, de.namelen) == 0;
                pos += de.reclen;
            }
            ++replies;
            din.cookie = din.cookie == 0 ? listing_cursor (out.snapshot_id, out.count) : din.cookie + out.count;
            if (out.count == 0)
                break;
        }
        while (replies < 601);
        valid &= seen == 600 && replies > 1 && directory_callbacks == (failure == 5 ? 3u : 2u) && directory_cached (f, in.nodeid);
        (last_native_result = usfs_client_destroy (&f));
        tap_ok (
            tap,
            valid,
            failure == 4 ? "callback failure discards the listing and retry caches a complete multi-reply scan"
                         : "directory growth failure reports ENOMEM, frees partial data and permits complete cached retry"
        );
    }
    {
        struct usfs_directory_sink db = { 0 };
        char name[USFS_MAX_NAME + 1];
        memset (name, 'x', sizeof (name) - 1);
        name[sizeof (name) - 1] = '\0';
        tap_ok (
            tap,
            usfs_directory_add (&db, name, NULL) < 0 && db.error == ENAMETOOLONG && usfs_directory_add (&db, "valid", NULL) < 0 && db.len == 0 &&
                db.data == NULL,
            "unrepresentable directory names fail the complete snapshot and construction errors remain sticky"
        );
        memset (&db, 0, sizeof (db));
        db.len = SIZE_MAX - 1;
        tap_ok (
            tap,
            usfs_directory_add (&db, "entry", NULL) < 0 && db.error == EOVERFLOW && db.len == SIZE_MAX - 1 && db.data == NULL,
            "directory length overflow fails before allocation or buffer access"
        );
    }
}

enum
{
    LARGE_DIRECTORY_ENTRIES = 140000
};

static int large_directory_listing (
    const struct usfs_client_request * callback_request,
    const char * path,
    struct usfs_open_file * file_info,
    struct usfs_directory_sink * sink
)
{
    (void)callback_request;
    (void)path;
    (void)file_info;
    ++directory_callbacks;

    for (unsigned entry_index = 0; entry_index < LARGE_DIRECTORY_ENTRIES; ++entry_index)
    {
        char name[32];
        snprintf (name, sizeof (name), "entry-%06u", entry_index);
        if (usfs_directory_add (sink, name, NULL))
            break;
    }

    return 0;
}

static void test_large_directory_spool (struct tap_state * tap)
{
    struct usfs_operations operations = { 0 };
    struct usfs_in_hdr header = { 0 };
    struct request_context context = { 0 };
    struct usfs_readdir_in request = { 0 };
    struct usfs_readdir_out reply = { 0 };
    unsigned entry_index = 0;
    unsigned windows = 0;
    uint64_t snapshot_id = 0;
    int valid = 1;

    operations.readdir = large_directory_listing;
    struct usfs_client * client = new_test_client (&operations, sizeof (operations), NULL);
    if (client == NULL)
        abort ();

    client->fd = 73;
    header.nodeid = USFS_ROOT_ID;
    header.uid = 501;
    context.client = client;
    context.header = &header;
    request.fh = 101;
    request.size = USFS_MAX_DATA;
    directory_test_handle (client, USFS_ROOT_ID, request.fh);
    directory_callbacks = directory_spool_opens = directory_spool_closes = 0;

    do
    {
        handle_readdir_request (&context, (char *)&request, sizeof (request));
        if (captured_reply.error != 0)
        {
            valid = 0;
            break;
        }

        memcpy (&reply, get_reply_buffer (client) + sizeof (struct usfs_reply_header), sizeof (reply));
        if (snapshot_id == 0)
            snapshot_id = reply.snapshot_id;
        valid &= reply.snapshot_id == snapshot_id;

        size_t position = sizeof (struct usfs_reply_header) + sizeof (reply);
        for (uint32_t item = 0; item < reply.count; ++item)
        {
            struct usfs_dirent entry;
            char expected[32];
            memcpy (&entry, get_reply_buffer (client) + position, sizeof (entry));
            snprintf (expected, sizeof (expected), "entry-%06u", entry_index++);
            valid &=
                entry.namelen == strlen (expected) && memcmp (get_reply_buffer (client) + position + sizeof (entry), expected, entry.namelen) == 0;
            position += entry.reclen;
        }

        request.cookie = listing_cursor (snapshot_id, entry_index);
        ++windows;
    }
    while (reply.count != 0 && windows < LARGE_DIRECTORY_ENTRIES);

    valid &= entry_index == LARGE_DIRECTORY_ENTRIES && windows > 2 && directory_callbacks == 1 && directory_spool_opens == 1 &&
             client->dircache_spool_bytes <= CLIENT_DIRSPOOL_MAX && client->dircache[0].spooled && client->dircache[0].checkpoint_count > 1;
    tap_ok (tap, valid, "large listing spills beyond 4 MiB and walks every entry in order with one backend enumeration");

    request.cookie = listing_cursor (snapshot_id, 60000);
    request.size = 1;
    handle_readdir_request (&context, (char *)&request, sizeof (request));
    valid = captured_reply.error == EINVAL;
    request.size = USFS_MAX_DATA;
    handle_readdir_request (&context, (char *)&request, sizeof (request));
    memcpy (&reply, get_reply_buffer (client) + sizeof (struct usfs_reply_header), sizeof (reply));
    struct usfs_dirent sought_entry;
    memcpy (&sought_entry, get_reply_buffer (client) + sizeof (struct usfs_reply_header) + sizeof (reply), sizeof (sought_entry));
    valid &= captured_reply.error == 0 && reply.snapshot_id == snapshot_id && reply.count > 0 &&
             memcmp (
                 get_reply_buffer (client) + sizeof (struct usfs_reply_header) + sizeof (reply) + sizeof (sought_entry),
                 "entry-060000",
                 sought_entry.namelen
             ) == 0 &&
             directory_callbacks == 1;
    tap_ok (tap, valid, "spooled cursor supports seek and a larger-buffer retry without re-enumeration");

    directory_spool_read_error = 1;
    handle_readdir_request (&context, (char *)&request, sizeof (request));
    valid = captured_reply.error == EIO && !directory_cached (client, USFS_ROOT_ID) && directory_spool_closes == directory_spool_opens &&
            client->dircache_spool_bytes == 0;
    directory_spool_read_error = 0;
    handle_readdir_request (&context, (char *)&request, sizeof (request));
    valid &= captured_reply.error == ESTALE;
    tap_ok (tap, valid, "spool read failure expires the cursor and releases charged storage");

    (last_native_result = usfs_client_destroy (&client));

    for (unsigned failure = 0; failure < 3; ++failure)
    {
        client = new_test_client (&operations, sizeof (operations), NULL);
        if (client == NULL)
            abort ();
        client->fd = 73;
        context.client = client;
        request.cookie = 0;
        directory_test_handle (client, USFS_ROOT_ID, request.fh);
        directory_spool_create_error = failure == 0;
        directory_spool_write_error = failure == 1;
        if (failure == 2)
            client->dircache_spool_bytes = CLIENT_DIRSPOOL_MAX;
        unsigned opens_before = directory_spool_opens;
        unsigned closes_before = directory_spool_closes;

        handle_readdir_request (&context, (char *)&request, sizeof (request));
        valid = captured_reply.error == (failure == 2 ? ENOSPC : EIO) && !directory_cached (client, USFS_ROOT_ID) &&
                directory_spool_opens - opens_before == directory_spool_closes - closes_before;
        if (failure != 2)
            valid &= client->dircache_spool_bytes == 0;
        tap_ok (tap, valid, "spool creation, write and quota failures discard the incomplete snapshot");

        directory_spool_create_error = directory_spool_write_error = 0;
        (last_native_result = usfs_client_destroy (&client));
    }

    free (directory_spool_data);
    directory_spool_data = NULL;
    directory_spool_length = directory_spool_capacity = 0;
}

static void test_directory_ram_bound (struct tap_state * tap)
{
    enum
    {
        RAM_BOUND_SNAPSHOT_COUNT = CLIENT_DIRCACHE_RAM_MAX / CLIENT_DIRCACHE_MAX + 1u
    };
    struct usfs_operations operations = { 0 };
    struct usfs_client * client = new_test_client (&operations, sizeof (operations), NULL);
    if (client == NULL)
        abort ();

    client->fd = 73;
    struct client_dircache_key key = { 0 };
    key.nodeid = USFS_ROOT_ID;
    uint64_t snapshot_ids[RAM_BOUND_SNAPSHOT_COUNT] = { 0 };
    int valid = 1;
    const unsigned initial_spool_opens = directory_spool_opens;
    const unsigned initial_spool_closes = directory_spool_closes;

    for (unsigned snapshot_index = 0; snapshot_index < RAM_BOUND_SNAPSHOT_COUNT; ++snapshot_index)
    {
        struct usfs_directory_sink directory_buffer = { 0 };
        directory_buffer.client = client;
        directory_buffer.auto_ino = CLIENT_SYNTHETIC_INODE_BASE + USFS_ROOT_ID;
        if (usfs_directory_add (&directory_buffer, "entry", NULL) != 0)
            abort ();

        char * data = realloc (directory_buffer.data, CLIENT_DIRCACHE_MAX);
        if (data == NULL)
            abort ();

        directory_buffer.data = data;
        directory_buffer.cap = CLIENT_DIRCACHE_MAX;
        valid &= publish_directory_snapshot (client, &key, &directory_buffer, &snapshot_ids[snapshot_index]) == 0;
        release_directory_buffer (&directory_buffer);
        valid &= client->dircache_ram_bytes <= CLIENT_DIRCACHE_RAM_MAX;
    }

    valid &= client->dircache_ram_bytes == CLIENT_DIRCACHE_RAM_MAX && client->dircache_spool_bytes > 0 &&
             client->dircache_spool_bytes <= CLIENT_DIRSPOOL_MAX && find_directory_cache_entry (client, &key, snapshot_ids[0]) != NULL &&
             find_directory_cache_entry (client, &key, snapshot_ids[RAM_BOUND_SNAPSHOT_COUNT - 1u]) != NULL &&
             client->dircache[RAM_BOUND_SNAPSHOT_COUNT - 1u].spooled && directory_spool_opens == initial_spool_opens + 1;

    (last_native_result = usfs_client_destroy (&client));
    valid &= directory_spool_closes == initial_spool_closes + 1;
    tap_ok (tap, valid, "RAM pressure spills a small snapshot while retaining earlier live cursors");

    free (directory_spool_data);
    directory_spool_data = NULL;
    directory_spool_length = directory_spool_capacity = 0;
}

/* Use the wire body directly so the same regression runs on version 5 and
 * demonstrates its missing release protocol without a production fallback. */
static int identity_forget (struct usfs_client * f, uint64_t id, uint64_t count, size_t length)
{
    char message[sizeof (struct usfs_in_hdr) + sizeof (count)];
    struct usfs_in_hdr in = { 0 };
    in.version = USFS_PROTOCOL_VERSION;
    in.opcode = 22;
    in.nodeid = id;
    in.unique = 99;
    in.len = sizeof (in) + length;
    memcpy (message, &in, sizeof (in));
    memcpy (message + sizeof (in), &count, sizeof (count));
    handle_request (f, message, in.len);
    return captured_reply.error;
}

static unsigned identity_nodes (struct usfs_client * f)
{
    struct client_node * node;
    unsigned count = 0;
    for (node = f->nodes; node != NULL; node = node->next)
        ++count;
    return count;
}

static void test_identity_reclamation (struct tap_state * tap)
{
    struct usfs_operations ops = { 0 };
    struct usfs_client * f = new_test_client (&ops, sizeof (ops), NULL);
    struct client_node *node, *parent, *child;
    uint64_t id, previous = 0;
    unsigned i;
    int valid = 1;
    f->fd = 73;
    for (i = 0; i < 2048; ++i)
    {
        char name[32];
        snprintf (name, sizeof (name), "unique-%u", i);
        node = find_or_create_node (f, 1, name, NULL);
        if (node == NULL)
            abort ();
        id = node->id;
        valid &= id > previous && identity_forget (f, id, 1, 8) == 0 && find_node_by_id (f, id) == NULL && identity_nodes (f) == 0;
        previous = id;
    }
    tap_ok (tap, valid, "unique-name churn reclaims object, aliases and locks and never reuses IDs");
    (last_native_result = usfs_client_destroy (&f));

    f = new_test_client (&ops, sizeof (ops), NULL);
    f->fd = 73;
    parent = find_or_create_node (f, 1, "parent", NULL);
    child = find_or_create_node (f, parent->id, "child", NULL);
    id = child->id;
    previous = parent->id;
    valid = identity_forget (f, previous, 1, 8) == 0 && find_node_by_id (f, previous) != NULL;
    {
        char path[USFS_PATH_MAX];
        valid &= build_node_path (f, id, path, sizeof (path)) == 0 && strcmp (path, "/parent/child") == 0;
    }
    valid &= identity_forget (f, id, 1, 8) == 0 && identity_nodes (f) == 0;
    tap_ok (tap, valid, "a retained child keeps its ancestor path and final release reclaims ancestors transitively");
    (last_native_result = usfs_client_destroy (&f));

    f = new_test_client (&ops, sizeof (ops), NULL);
    f->fd = 73;
    {
        struct stat st = { 0 };
        st.st_mode = S_IFREG | 0600;
        st.st_ino = 9;
        st.st_dev = 7;
        st.st_nlink = 2;
        node = find_or_create_node (f, 1, "a", &st);
        id = node->id;
        valid = find_or_create_node (f, 1, "b", &st) == node;
        valid &= identity_forget (f, id, 1, 8) == 0 && find_node_by_id (f, id) != NULL && find_child_node (f, 1, "b") == node;
        valid &= identity_forget (f, id, 1, 8) == 0 && identity_nodes (f) == 0 && find_child_node (f, 1, "a") == NULL &&
                 find_node_by_backend_identity (f, &st) == NULL;
        node = find_or_create_node (f, 1, "a", &st);
        valid &= node->id > id;
        tap_ok (tap, valid, "hard-link lookups balance on one object and retired records cannot revive an old ID");
    }
    (last_native_result = usfs_client_destroy (&f));

    for (i = 0; i < 5; ++i)
    {
        f = new_test_client (&ops, sizeof (ops), NULL);
        f->fd = 73;
        node = find_or_create_node (f, 1, "held", NULL);
        id = node->id;
        valid = identity_forget (f, i == 4 ? id + 1 : id, i == 0 ? 0 : i == 1 ? 2 : i == 2 ? UINT64_MAX : 1, i == 3 ? 7 : 8) == EINVAL;
        valid &= find_node_by_id (f, id) == node && identity_forget (f, id, 1, 8) == 0 && identity_nodes (f) == 0;
        tap_ok (tap, valid, "malformed, excessive or unknown FORGET cannot consume legitimate ownership");
        (last_native_result = usfs_client_destroy (&f));
    }
    ops.open = flags_open;
    ops.release = flags_release;
    f = new_test_client (&ops, sizeof (ops), NULL);
    f->fd = 73;
    node = find_or_create_node (f, 1, "held", NULL);
    id = node->id;
    {
        struct usfs_in_hdr in = { 0 };
        struct request_context context = { 0 };

        context.header = &in;
        context.client = f;

        struct usfs_open_in open_in = { 0 };
        struct usfs_release_in release = { 0 };
        char message[sizeof (in) + sizeof (release)];
        in.nodeid = id;
        open_in.flags = FREAD;
        release_calls = 0;
        handle_open_request (&context, (char *)&open_in, sizeof (open_in));
        remove_alias (f, node->aliases);
        valid = identity_forget (f, id, 1, 8) == 0 && find_node_by_id (f, id) == node;
        release.fh = 456;
        release.flags = FREAD;
        in.opcode = USFS_OP_RELEASE;
        in.version = USFS_PROTOCOL_VERSION;
        in.len = sizeof (message);
        in.unique = 100;
        memcpy (message, &in, sizeof (in));
        memcpy (message + sizeof (in), &release, sizeof (release));
        handle_request (f, message, sizeof (message));
        valid &= captured_reply.error == 0 && identity_nodes (f) == 0 && release_calls == 1;
        tap_ok (tap, valid, "an open unlinked object survives FORGET and final RELEASE reclaims it exactly once");
    }
    (last_native_result = usfs_client_destroy (&f));

    memset (&ops, 0, sizeof (ops));
    f = new_test_client (&ops, sizeof (ops), NULL);
    f->fd = 73;
    valid = 1;
    for (i = 0; i < 64; ++i)
    {
        char name[32];
        struct usfs_directory_sink db = { 0 };
        struct client_dircache_key key = { 0 };
        uint64_t snapshot_id = 0;
        snprintf (name, sizeof (name), "directory-%u", i);
        node = find_or_create_node (f, 1, name, NULL);
        id = node->id;
        directory_test_handle (f, id, id);
        key.nodeid = key.fh = id;
        key.generation = f->handles->generation;
        db.client = f;
        db.data = malloc (8);
        db.len = 8;
        db.cap = 8;
        if (db.data == NULL)
            abort ();
        valid &= publish_directory_snapshot (f, &key, &db, &snapshot_id) == 0 && snapshot_id != 0;
        release_directory_buffer (&db);
        valid &= identity_forget (f, id, 1, 8) == 0 && find_node_by_id (f, id) != NULL && directory_cached (f, id);
        struct client_handle * closed = f->handles;
        f->handles = closed->next;
        dispose_client_handle (f, closed);
        collect_unreferenced_nodes (f);
        valid &= identity_nodes (f) == 0 && !directory_cached (f, id);
    }
    tap_ok (tap, valid, "directory release drops cached ownership and reclaims unused identities");
    (last_native_result = usfs_client_destroy (&f));

    ops.open = flags_open;
    ops.release = flags_release;
    f = new_test_client (&ops, sizeof (ops), NULL);
    f->fd = 73;
    node = find_or_create_node (f, 1, "file", NULL);
    {
        struct usfs_in_hdr in = { 0 };
        struct request_context context = { 0 };

        context.header = &in;
        context.client = f;

        struct usfs_open_in open_in = { 0 };
        in.nodeid = node->id;
        open_in.flags = FREAD;
        open_calls = release_calls = 0;
        fail_client_calloc = 1;
        handle_open_request (&context, (char *)&open_in, sizeof (open_in));
        fail_client_calloc = 0;
        tap_ok (tap, captured_reply.error == ENOMEM && open_calls == 0, "OPEN reserves cleanup ownership before acquiring a backend handle");
        reply_write_error = EIO;
        handle_open_request (&context, (char *)&open_in, sizeof (open_in));
        reply_write_error = 0;
        valid = f->session.exited;
        (last_native_result = usfs_client_destroy (&f));
        tap_ok (tap, valid && release_calls == 1, "reply delivery failure stops the session and destruction releases every retained backend handle");
    }
    memfs_fixture ();
    memset (&ops, 0, sizeof (ops));
    ops.create_attr = memfs_create_attr;
    f = new_test_client (&ops, sizeof (ops), NULL);
    f->fd = 73;
    {
        struct usfs_in_hdr in = { 0 };
        struct request_context context = { 0 };

        context.header = &in;
        context.client = f;

        struct usfs_create_attr_in create = { 0 };
        char body[sizeof (create) + 4];
        in.nodeid = 1;
        create.flags = FREAD;
        create.activation = USFS_CREATE_OPEN;
        create.attr.valid = USFS_SET_MODE;
        create.attr.mode = 0600;
        memcpy (body, &create, sizeof (create));
        memcpy (body + sizeof (create), "new", 4);
        fail_client_calloc = 3;
        memfs_calls = 0;
        handle_create_attr_request (&context, body, sizeof (body));
        fail_client_calloc = 0;
        valid = captured_reply.error == ENOMEM && memfs_calls == 0 && resolve ("/new", NULL, NULL) == NULL;
        (last_native_result = usfs_client_destroy (&f));
        valid &= memfs_fixture_clean ();
        tap_ok (tap, valid, "atomic CREATE reserves handle ownership before backend publication");
    }

#if USFS_PROTOCOL_VERSION >= 6
    {
        pthread_t reader, forgetter;
        struct ns_job read, forget;
        memset (&ops, 0, sizeof (ops));
        ops.getattr = ns_getattr;
        f = new_test_client (&ops, sizeof (ops), NULL);
        f->fd = 73;
        node = find_or_create_node (f, 1, "independent", NULL);
        id = node->id;
        read = (struct ns_job){ f, id, 0 };
        forget = (struct ns_job){ f, id, 2 };
        memset (&ns_test, 0, sizeof (ns_test));
        ns_test.active = ns_test.block_read = 1;
        ns_start (&reader, &read);
        pthread_mutex_lock (&ns_test_lock);
        ns_wait_for (&ns_test.reads, 1);
        pthread_mutex_unlock (&ns_test_lock);
        ns_start (&forgetter, &forget);
        pthread_mutex_lock (&ns_test_lock);
        ns_wait_for (&ns_test.writer_events, 1);
        valid = find_node_by_id (f, id) != NULL;
        ns_test.release_read = 1;
        pthread_cond_broadcast (&ns_test_changed);
        pthread_mutex_unlock (&ns_test_lock);
        pthread_join (reader, NULL);
        pthread_join (forgetter, NULL);
        valid &= ns_test.errors == 0 && identity_nodes (f) == 0;
        ns_test.active = 0;
        (last_native_result = usfs_client_destroy (&f));
        tap_ok (tap, valid, "FORGET waits for the active callback and reclaims only after reply completion");

        f = new_test_client (&ops, sizeof (ops), NULL);
        f->fd = 73;
        node = find_or_create_node (f, 1, "independent", NULL);
        id = node->id;
        read = (struct ns_job){ f, 1, 3 };
        forget = (struct ns_job){ f, id, 2 };
        memset (&ns_test, 0, sizeof (ns_test));
        ns_test.active = ns_test.block_reply = 1;
        ns_start (&reader, &read);
        pthread_mutex_lock (&ns_test_lock);
        ns_wait_for (&ns_test.reply_entered, 1);
        pthread_mutex_unlock (&ns_test_lock);
        ns_start (&forgetter, &forget);
        pthread_mutex_lock (&ns_test_lock);
        ns_wait_for (&ns_test.writer_events, 1);
        ns_test.release_reply = 1;
        pthread_cond_broadcast (&ns_test_changed);
        pthread_mutex_unlock (&ns_test_lock);
        pthread_join (reader, NULL);
        pthread_join (forgetter, NULL);
        valid = ns_test.errors == 0 && find_node_by_id (f, id) != NULL;
        ns_test.active = 0;
        valid &= identity_forget (f, id, 1, 8) == 0 && identity_nodes (f) == 0;
        (last_native_result = usfs_client_destroy (&f));
        tap_ok (tap, valid, "lookup reacquisition during final FORGET preserves the newly transferred reference");
    }
#else
    tap_ok (tap, 0, "FORGET waits for the active callback and reclaims only after reply completion");
    tap_ok (tap, 0, "lookup reacquisition during final FORGET preserves the newly transferred reference");
#endif
}

struct lifecycle_context_data
{
    struct usfs_client * f;
    void *initial, *after;
    unsigned init, release, releasedir, destroy;
    int errors;
};
static struct lifecycle_context_data * lifecycle_expected;

static int context_init (const struct usfs_client_request * request, const struct usfs_limits * limits, struct usfs_behavior * behavior)
{
    (void)limits;
    (void)behavior;
    struct lifecycle_context_data * data = lifecycle_expected;
    if (usfs_request_client (request) != data->f || usfs_request_user_data (request) != data->initial || usfs_request_uid (request) ||
        usfs_request_gid (request) || usfs_request_pid (request))
        ++data->errors;
    ++data->init;
    return 0;
}
static void context_check_cleanup (void)
{
    struct usfs_client_request * context = get_callback_request ();
    struct lifecycle_context_data * data = lifecycle_expected;
    if (context == NULL || context->client != data->f || context->user_data != data->initial || context->uid || context->gid || context->pid)
        ++data->errors;
}
static int context_release (const struct usfs_client_request * callback_request, const char * path, struct usfs_open_file * fi)
{
    (void)callback_request;
    (void)path;
    (void)fi;
    context_check_cleanup ();
    ++lifecycle_expected->release;
    return 0;
}
static int context_releasedir (const struct usfs_client_request * callback_request, const char * path, struct usfs_open_file * fi)
{
    (void)callback_request;
    (void)path;
    (void)fi;
    context_check_cleanup ();
    ++lifecycle_expected->releasedir;
    return 0;
}
static void context_destroy (const struct usfs_client_request * request)
{
    context_check_cleanup ();
    if (usfs_request_user_data (request) != lifecycle_expected->initial)
        ++lifecycle_expected->errors;
    ++lifecycle_expected->destroy;
}

static void * context_thread (void * opaque)
{
    struct lifecycle_context_data * data = opaque;
    struct client_context_scope scope = { 0 };
    if (get_callback_request () != NULL || enter_callback_context (data->f, &scope) != 0)
    {
        ++data->errors;
        return NULL;
    }
    for (unsigned i = 0; i < 1000; ++i)
    {
        struct usfs_client_request * context = get_callback_request ();
        if (context == NULL || context->client != data->f || context->user_data != data->initial)
            ++data->errors;
        sched_yield ();
    }
    restore_callback_context (&scope);
    if (get_callback_request () != NULL)
        ++data->errors;
    return NULL; /* Context storage is freed by the pthread-key destructor. */
}

static void test_lifecycle_context (struct tap_state * tap)
{
    struct usfs_operations ops = { 0 };
    int initial, after;
    ops.initialize = context_init;
    ops.release = context_release;
    ops.releasedir = context_releasedir;
    ops.shutdown = context_destroy;
    for (unsigned scenario = 0; scenario < 4; ++scenario)
    {
        struct lifecycle_context_data data = { 0 };
        data.initial = scenario & 2 ? NULL : &initial;
        data.after = data.initial;
        data.f = new_test_client (&ops, sizeof (ops), data.initial);
        if (data.f == NULL)
            abort ();
        lifecycle_expected = &data;
        usfs_client_request_stop (data.f); /* Exercise initialization without a device read. */
        int rc = scenario & 1 ? run_multithreaded_session (data.f, 2) : run_singlethreaded_session (data.f);
        struct client_handle *file = calloc (1, sizeof (*file)), *dir = calloc (1, sizeof (*dir));
        if (file == NULL || dir == NULL)
            abort ();
        file->nodeid = dir->nodeid = USFS_ROOT_ID;
        file->fh = 1;
        dir->fh = 2;
        dir->isdir = 1;
        file->next = dir;
        data.f->handles = file;
        int valid = rc == 0 && get_callback_request () == NULL && data.f->user_data == data.initial;
        (last_native_result = usfs_client_destroy (&data.f));
        tap_ok (
            tap,
            valid && data.errors == 0 && data.init == 1 && data.release == 1 && data.releasedir == 1 && data.destroy == 1 &&
                get_callback_request () == NULL,
            "both loops establish lifecycle context and clean leftover handles with the unchanged application data"
        );
    }
    {
        struct usfs_client outer = { 0 }, inner = { 0 };
        struct client_context_scope first = { 0 }, second = { 0 };
        outer.user_data = &initial;
        inner.user_data = &after;
        if (enter_callback_context (&outer, &first) != 0)
            abort ();
        get_callback_request ()->uid = 123;
        if (enter_callback_context (&inner, &second) != 0)
            abort ();
        int valid = get_callback_request ()->client == &inner && get_callback_request ()->uid == 0;
        restore_callback_context (&second);
        valid &= get_callback_request ()->client == &outer && get_callback_request ()->user_data == &initial && get_callback_request ()->uid == 123;
        restore_callback_context (&first);
        tap_ok (tap, valid && get_callback_request () == NULL, "nested context scopes restore the complete caller context");
    }
    {
        struct usfs_client instances[2] = { { 0 }, { 0 } };
        struct lifecycle_context_data data[2] = { { 0 }, { 0 } };
        pthread_t workers[2];
        for (unsigned i = 0; i < 2; ++i)
        {
            instances[i].user_data = &data[i];
            data[i].f = &instances[i];
            data[i].initial = &data[i];
            if (pthread_create (&workers[i], NULL, context_thread, &data[i]) != 0)
                abort ();
        }
        for (unsigned i = 0; i < 2; ++i)
            pthread_join (workers[i], NULL);
        tap_ok (
            tap,
            data[0].errors == 0 && data[1].errors == 0 && get_callback_request () == NULL,
            "independent threads cannot observe another filesystem context"
        );
    }
    for (unsigned fault = 0; fault < 2; ++fault)
    {
        struct lifecycle_context_data data = { 0 };
        data.initial = &initial;
        data.after = &after;
        data.f = new_test_client (&ops, sizeof (ops), data.initial);
        if (data.f == NULL)
            abort ();
        lifecycle_expected = &data;
        struct client_thread_context * saved = get_thread_context ();
        pthread_key_t saved_key = client_context_key;
        pthread_setspecific (client_context_key, NULL);
        if (fault)
        {
            __atomic_store_n (&client_context_key_ready, 0, __ATOMIC_RELEASE);
            fail_context_key_create = 1;
        }
        else
            fail_client_calloc = 1;
        int expected = fault ? EAGAIN : ENOMEM;
        int rc = run_singlethreaded_session (data.f);
        int valid = rc == -expected && !data.f->init_done && data.init == 0;
        if (!fault)
            fail_client_calloc = 1;
        valid &= run_multithreaded_session (data.f, 2) == -expected && !data.f->init_done && data.init == 0;
        data.f->init_done = 1; /* Require a destruction callback on the retry path. */
        if (!fault)
            fail_client_calloc = 1;
        valid &= run_singlethreaded_session (data.f) == -expected;
        if (!fault)
            fail_client_calloc = 1;
        valid &= run_multithreaded_session (data.f, 2) == -expected;
        if (!fault)
            fail_client_calloc = 1;
        (last_native_result = usfs_client_destroy (&data.f));
        valid &= last_native_result == -expected && data.destroy == 0 && data.f->user_data == data.initial;
        fail_context_key_create = 0;
        fail_client_calloc = 0;
        data.f->user_data = data.initial;
        (last_native_result = usfs_client_destroy (&data.f));
        struct client_thread_context * retried = get_thread_context ();
        valid &= retried != NULL;
        pthread_setspecific (client_context_key, NULL);
        free (retried);
        if (fault)
        {
            pthread_key_delete (client_context_key);
            client_context_key = saved_key;
            __atomic_store_n (&client_context_key_ready, 1, __ATOMIC_RELEASE);
        }
        pthread_setspecific (client_context_key, saved);
        tap_ok (tap, valid && data.errors == 0 && data.destroy == 1, "context setup failure precedes callbacks and destruction remains retryable");
    }
}

static void test_loop_failures (struct tap_state * tap)
{
    for (unsigned mt = 0; mt < 2; ++mt)
        for (unsigned scenario = 0; scenario < 8; ++scenario)
        {
            struct usfs_operations ops = { 0 };
            struct usfs_in_hdr in = { 0 };
            struct usfs_open_in open_in = { 0 };
            ops.open = flags_open;
            ops.release = flags_release;
            struct usfs_client * f = new_test_client (&ops, sizeof (ops), NULL);
            if (f == NULL)
                abort ();
            f->fd = 73;
            f->mounted = 1;
            f->mountpoint = strdup ("/canonical/mount");
            f->info.fs_type = 37;
            f->info.cookie = 123;
            struct client_node * node = find_or_create_node (f, 1, "file", NULL);
            if (node == NULL)
                abort ();
            in.nodeid = node->id;
            in.opcode = USFS_OP_OPEN;
            in.unique = 1;
            in.version = USFS_PROTOCOL_VERSION;
            in.len = sizeof (loop_message);
            open_in.flags = FREAD;
            memcpy (loop_message, &in, sizeof (in));
            memcpy (loop_message + sizeof (in), &open_in, sizeof (open_in));
            loop_message_sent = 0;
            loop_read_error = scenario >= 2 && scenario <= 4 ? EIO : 0;
            reply_write_error = scenario == 0 ? ENOSPC : 0;
            open_calls = release_calls = 0;
            mount_records_size = 0;
            mount_records_count = query_error = query_grows = 0;
            add_mount_record (11, scenario == 2 ? 456 : 123, 0, f->mountpoint);
            if (scenario == 1)
                usfs_client_request_stop (f);
            if (scenario == 4)
                query_error = 1;
            if (scenario == 5)
            {
                fail_session (f, ENOMEM);
                fail_session (f, EIO);
                usfs_client_request_stop (f);
            }
            loop_eof = scenario >= 6;
            if (loop_eof)
                query_error = 1; /* Inventory can still include the unmounting filesystem. */
            if (scenario == 7)
                f->session.fatal_error = ENOMEM;
            int result = mt ? run_multithreaded_session (f, 2) : run_singlethreaded_session (f);
            int expected = scenario == 0 ? -ENOSPC : scenario == 1 || scenario == 6 ? 0 : scenario == 5 || scenario == 7 ? -ENOMEM : -EIO;
            int eof_valid = !loop_eof || (is_session_exited (&f->session) && f->mounted);
            loop_eof = 0;
            reply_write_error = loop_read_error = query_error = 0;
            (last_native_result = usfs_client_unmount (f));
            int valid = result == expected && !f->mounted && eof_valid;
            (last_native_result = usfs_client_destroy (&f));
            valid &= open_calls == (scenario == 0 ? 1u : 0u) && release_calls == open_calls;
            tap_ok (tap, valid, "both daemon loops distinguish fatal errors from orderly exit and release handles once");
        }
}

static const char * forced_identity_path;
static int force_unterminated_identity_path;

static int resolve_test_identity (const struct usfs_client_request * callback_request, const uint64_t token, char * path, const size_t path_capacity)
{
    (void)callback_request;
    if (force_unterminated_identity_path)
    {
        memset (path, 'x', path_capacity);
        return 0;
    }

    if (forced_identity_path == NULL)
        return resolve_object_id (get_callback_request (), token, path, path_capacity);

    const size_t length = strlen (forced_identity_path) + 1u;
    if (length > path_capacity)
        return -ENAMETOOLONG;

    memcpy (path, forced_identity_path, length);
    return 0;
}

static int request_fid (struct usfs_client * client, const uint64_t nodeid, uint64_t * token)
{
    struct usfs_in_hdr header = { 0 };
    struct request_context context = { 0 };

    header.nodeid = nodeid;
    context.header = &header;
    context.client = client;

    handle_fid_request (&context, 0);
    if (captured_reply.error == 0 && token != NULL)
    {
        struct usfs_fid_out reply;
        memcpy (&reply, get_reply_buffer (client) + sizeof (struct usfs_reply_header), sizeof (reply));
        *token = reply.token;
    }

    return captured_reply.error;
}

static int request_vget (struct usfs_client * client, const uint64_t token, uint64_t * nodeid)
{
    struct usfs_in_hdr header = { 0 };
    struct request_context context = { 0 };
    struct usfs_vget_in request = { 0 };

    header.nodeid = USFS_ROOT_ID;
    context.header = &header;
    context.client = client;
    request.token = token;

    handle_vget_request (&context, (const char *)&request, sizeof (request));
    if (captured_reply.error == 0 && nodeid != NULL)
    {
        struct usfs_vget_out reply;
        memcpy (&reply, get_reply_buffer (client) + sizeof (struct usfs_reply_header), sizeof (reply));
        *nodeid = reply.entry.nodeid;
    }

    return captured_reply.error;
}

static struct usfs_client * new_identity_client (const struct usfs_operations * operations, const size_t operations_size)
{
    struct usfs_client * client = new_test_client (operations, operations_size, NULL);
    if (client == NULL)
        abort ();

    client->fd = 73;
    return client;
}

static void test_fid_callback_pair (struct tap_state * tap)
{
    struct usfs_operations operations = { 0 };
    operations.getattr = get_file_attributes;
    operations.export_id = export_object_id;
    operations.resolve_id = resolve_object_id;

    struct usfs_client * client = new_identity_client (&operations, offsetof (struct usfs_operations, export_id));
    int valid = request_fid (client, USFS_ROOT_ID, NULL) == EOPNOTSUPP && request_vget (client, 1, NULL) == EOPNOTSUPP;
    tap_ok (tap, valid, "short native operation tables omit identity callbacks");
    (last_native_result = usfs_client_destroy (&client));

    client = new_test_client (&operations, offsetof (struct usfs_operations, export_id) + 1u, NULL);
    valid = client == NULL && last_native_result == -EINVAL;
    tap_ok (tap, valid, "partial native export_id member is rejected");
    (last_native_result = usfs_client_destroy (&client));

    client = new_test_client (&operations, offsetof (struct usfs_operations, resolve_id) + 1u, NULL);
    valid = client == NULL && last_native_result == -EINVAL;
    tap_ok (tap, valid, "partial native resolve_id member is rejected");
    (last_native_result = usfs_client_destroy (&client));

    operations.resolve_id = NULL;
    client = new_identity_client (&operations, sizeof (operations));
    valid = request_fid (client, USFS_ROOT_ID, NULL) == EOPNOTSUPP && request_vget (client, 1, NULL) == EOPNOTSUPP;
    tap_ok (tap, valid, "export-only callbacks do not claim persistent identity support");
    (last_native_result = usfs_client_destroy (&client));

    operations.export_id = NULL;
    operations.resolve_id = resolve_object_id;
    client = new_identity_client (&operations, sizeof (operations));
    valid = request_fid (client, USFS_ROOT_ID, NULL) == EOPNOTSUPP && request_vget (client, 1, NULL) == EOPNOTSUPP;
    tap_ok (tap, valid, "resolve-only callbacks do not claim persistent identity support");
    (last_native_result = usfs_client_destroy (&client));
}

static void test_fid_vget_reconstruction (struct tap_state * tap)
{
    memfs_fixture ();

    struct usfs_open_file file_info = { 0 };
    file_info.open_flags = O_RDWR;
    if (memfs_mkdir (get_callback_request (), "/dir", 0755) != 0 || memfs_create (get_callback_request (), "/dir/file", 0644, &file_info) != 0)
        abort ();

    if (memfs_release (get_callback_request (), "/dir/file", &file_info) != 0)
        abort ();

    struct stat metadata;
    if (get_file_attributes (get_callback_request (), "/dir/file", &metadata, NULL) != 0)
        abort ();

    struct usfs_operations operations = { 0 };
    operations.getattr = get_file_attributes;
    operations.export_id = export_object_id;
    operations.resolve_id = resolve_test_identity;
    struct usfs_client * client = new_identity_client (&operations, sizeof (operations));

    uint64_t root_token = 0;
    uint64_t root_nodeid = 0;
    int valid = request_fid (client, USFS_ROOT_ID, &root_token) == 0 && root_token == g_root->ino &&
                request_vget (client, root_token, &root_nodeid) == 0 && root_nodeid == USFS_ROOT_ID && client->root_lookup_refs == 1;
    valid &= identity_forget (client, USFS_ROOT_ID, 1, sizeof (struct usfs_forget_in)) == 0 && client->root_lookup_refs == 0;
    tap_ok (tap, valid, "root FID and VGET transfer one root lookup reference");

    uint64_t first_nodeid = 0;
    valid = request_vget (client, metadata.st_ino, &first_nodeid) == 0 && first_nodeid > USFS_ROOT_ID;
    struct client_node * directory = find_child_node (client, USFS_ROOT_ID, "dir");
    struct client_node * file = find_node_by_id (client, first_nodeid);
    valid &= directory != NULL && directory->lookup_refs == 0 && file != NULL && file->lookup_refs == 1 &&
             find_child_node (client, directory->id, "file") == file;
    tap_ok (tap, valid, "cold VGET reconstructs ancestors and keeps only the target lookup reference");

    valid = identity_forget (client, first_nodeid, 1, sizeof (struct usfs_forget_in)) == 0 && find_node_by_id (client, first_nodeid) == NULL;
    uint64_t second_nodeid = 0;
    valid &= request_vget (client, metadata.st_ino, &second_nodeid) == 0 && second_nodeid > first_nodeid;
    tap_ok (tap, valid, "VGET reconstructs the backend object after node eviction");

    if (memfs_link (get_callback_request (), "/dir/file", "/alias") != 0)
        abort ();

    uint64_t alias_nodeid = 0;
    valid = request_vget (client, metadata.st_ino, &alias_nodeid) == 0 && alias_nodeid == second_nodeid;
    file = find_node_by_id (client, second_nodeid);
    valid &= file != NULL && file->lookup_refs == 2 && find_child_node (client, USFS_ROOT_ID, "alias") == file;
    tap_ok (tap, valid, "VGET through a hard-link alias preserves canonical node identity");

    const uint64_t refs_before_invalid = file != NULL ? file->lookup_refs : 0;
    static const char * malformed_paths[] = { "relative", "/dir//file", "/dir/../file", "/dir/./file", "/dir/file/" };
    valid = 1;
    for (size_t index = 0; index < sizeof (malformed_paths) / sizeof (malformed_paths[0]); index++)
    {
        forced_identity_path = malformed_paths[index];
        valid &= request_vget (client, metadata.st_ino, NULL) == EIO;
    }

    forced_identity_path = NULL;
    force_unterminated_identity_path = 1;
    valid &= request_vget (client, metadata.st_ino, NULL) == EIO;
    force_unterminated_identity_path = 0;
    valid &= file != NULL && file->lookup_refs == refs_before_invalid;
    tap_ok (tap, valid, "VGET rejects malformed or unterminated resolver paths without retaining references");

    forced_identity_path = "/";
    valid = request_vget (client, metadata.st_ino, NULL) == ESTALE && file != NULL && file->lookup_refs == refs_before_invalid;
    forced_identity_path = NULL;
    tap_ok (tap, valid, "VGET rejects a path whose exported token differs from the request");

    valid = identity_forget (client, second_nodeid, 2, sizeof (struct usfs_forget_in)) == 0 && identity_nodes (client) == 0;
    valid &= memfs_unlink (get_callback_request (), "/dir/file") == 0 && memfs_unlink (get_callback_request (), "/alias") == 0 &&
             remove_directory (get_callback_request (), "/dir") == 0;
    (last_native_result = usfs_client_destroy (&client));
    valid &= memfs_fixture_clean ();
    tap_ok (tap, valid, "identity reconstruction leaves backend and client resources reclaimable");
}

static void test_fid_detached_alias (struct tap_state * tap)
{
    memfs_fixture ();

    struct usfs_open_file file_info = { 0 };
    file_info.open_flags = O_RDWR;
    if (memfs_create (get_callback_request (), "/known", 0644, &file_info) != 0 ||
        memfs_release (get_callback_request (), "/known", &file_info) != 0 || memfs_link (get_callback_request (), "/known", "/hidden") != 0)
        abort ();

    struct stat metadata;
    if (get_file_attributes (get_callback_request (), "/known", &metadata, NULL) != 0)
        abort ();

    struct usfs_operations operations = { 0 };
    operations.getattr = get_file_attributes;
    operations.export_id = export_object_id;
    operations.resolve_id = resolve_object_id;
    struct usfs_client * client = new_identity_client (&operations, sizeof (operations));
    struct client_node * node = find_or_create_node (client, USFS_ROOT_ID, "known", &metadata);
    if (node == NULL)
        abort ();

    const uint64_t original_nodeid = node->id;
    if (memfs_unlink (get_callback_request (), "/known") != 0)
        abort ();

    publish_removed_entry (client, USFS_ROOT_ID, "known");
    uint64_t token = 0;
    int valid =
        node->detached && node->backend_links == 1 && request_fid (client, original_nodeid, &token) == 0 && token == (uint64_t)metadata.st_ino;
    tap_ok (tap, valid, "FID exports a detached node while an unknown hard link remains live");

    uint64_t recovered_nodeid = 0;
    valid = request_vget (client, token, &recovered_nodeid) == 0 && recovered_nodeid == original_nodeid &&
            find_child_node (client, USFS_ROOT_ID, "hidden") == node;
    tap_ok (tap, valid, "VGET resolves an untracked hard link to the retained canonical node");

    if (memfs_unlink (get_callback_request (), "/hidden") != 0)
        abort ();

    publish_removed_entry (client, USFS_ROOT_ID, "hidden");
    valid = request_fid (client, original_nodeid, NULL) == ESTALE && request_vget (client, token, NULL) == ESTALE;
    valid &= identity_forget (client, original_nodeid, 2, sizeof (struct usfs_forget_in)) == 0 && identity_nodes (client) == 0;
    tap_ok (tap, valid, "final link removal makes FID and VGET stale");

    (last_native_result = usfs_client_destroy (&client));
    if (!memfs_fixture_clean ())
        abort ();
}

static volatile sig_atomic_t app_signal_count;

static void record_app_signal (int signal_number)
{
    (void)signal_number;
    app_signal_count++;
}

static void record_later_signal (int signal_number)
{
    (void)signal_number;
}

static int signal_action_is (int signal_number, void (*handler) (int), int mask_member)
{
    struct sigaction current = { 0 };

    if (sigaction (signal_number, NULL, &current) != 0)
        return 0;

    return current.sa_handler == handler && sigismember (&current.sa_mask, SIGUSR1) == mask_member;
}

static int child_signal_delivery_matches (const struct client_session * owner, const struct client_session * nonowner, const int expect_app_handler)
{
    const pid_t child = fork ();
    if (child == -1)
        return 0;

    if (child == 0)
    {
        alarm (2);
        if (raise (SIGTERM) != 0)
            _exit (1);

        const int valid = expect_app_handler ? app_signal_count == 1 : owner->exited && !nonowner->exited;
        _exit (valid ? 0 : 1);
    }

    int status = 0;
    pid_t waited;
    do
    {
        waited = waitpid (child, &status, 0);
    }
    while (waited == -1 && errno == EINTR);

    return waited == child && WIFEXITED (status) && WEXITSTATUS (status) == 0;
}

struct signal_registration_job
{
    struct client_session * session; // Session competing to own the handlers.
    pthread_mutex_t * lock;          // Shared barrier mutex.
    pthread_cond_t * changed;        // Shared barrier condition variable.
    unsigned * ready;                // Number of registration threads at the barrier.
    int * start;                     // Whether the barrier has been released.
    int result;                      // Registration result from this thread.
    int error;                       // Errno captured after registration.
};

static void * register_signal_owner (void * opaque)
{
    struct signal_registration_job * job = opaque;

    pthread_mutex_lock (job->lock);
    ++*job->ready;
    pthread_cond_broadcast (job->changed);
    while (!*job->start)
        pthread_cond_wait (job->changed, job->lock);
    pthread_mutex_unlock (job->lock);

    job->result = install_signal_handlers (job->session);
    job->error = errno;

    return NULL;
}

static void test_signal_ownership (struct tap_state * tap)
{
    static const int signal_numbers[] = { SIGHUP, SIGINT, SIGTERM, SIGPIPE };
    struct sigaction original_actions[4] = { 0 };
    struct sigaction app_action = { 0 };
    struct client_session first = { 0 };
    struct client_session second = { 0 };
    int valid = 1;

    app_action.sa_handler = record_app_signal;
    sigemptyset (&app_action.sa_mask);
    sigaddset (&app_action.sa_mask, SIGUSR1);
    for (unsigned index = 0; index < 4; ++index)
    {
        if (sigaction (signal_numbers[index], &app_action, &original_actions[index]) != 0)
            abort ();
    }

    first.wake_write = second.wake_write = 75;

    errno = 0;
    valid = install_signal_handlers (NULL) == -1 && errno == EINVAL && signal_session == NULL;
    tap_ok (tap, valid, "signal registration rejects a null session without changing ownership");

    valid = install_signal_handlers (&first) == 0 && install_signal_handlers (&first) == 0;
    errno = 0;
    valid &= install_signal_handlers (&second) == -1 && errno == EBUSY && signal_session == &first;
    tap_ok (tap, valid, "one signal owner rejects a second session and duplicate registration is idempotent");

    remove_signal_handlers (&second);
    valid = signal_session == &first && signal_action_is (SIGTERM, handle_exit_signal, 0);
    valid &= child_signal_delivery_matches (&first, &second, 0);
    tap_ok (tap, valid, "removing a nonowner leaves the active handler and owner intact");

    remove_signal_handlers (&first);
    valid = signal_session == NULL;
    for (unsigned index = 0; index < 4; ++index)
        valid &= signal_action_is (signal_numbers[index], record_app_signal, 1);
    valid &= install_signal_handlers (&second) == 0;
    tap_ok (tap, valid, "owner removal restores application dispositions and permits the next owner");

    struct usfs_operations unrelated_operations = { 0 };
    struct usfs_client * unrelated = new_test_client (&unrelated_operations, sizeof (unrelated_operations), NULL);
    if (unrelated == NULL)
        abort ();
    (last_native_result = usfs_client_destroy (&unrelated));
    valid = signal_session == &second && signal_action_is (SIGTERM, handle_exit_signal, 0);
    remove_signal_handlers (&second);
    tap_ok (tap, valid, "closing an unrelated session cannot remove the current signal owner");

    if (install_signal_handlers (&first) != 0)
        abort ();
    struct sigaction later_action = { 0 };
    later_action.sa_handler = record_later_signal;
    sigemptyset (&later_action.sa_mask);
    if (sigaction (SIGINT, &later_action, NULL) != 0)
        abort ();
    remove_signal_handlers (&first);
    valid = signal_action_is (SIGINT, record_later_signal, 0) && signal_action_is (SIGTERM, record_app_signal, 1) && signal_session == NULL;
    tap_ok (tap, valid, "removal preserves an application handler installed after registration");
    if (sigaction (SIGINT, &app_action, NULL) != 0)
        abort ();

    if (install_signal_handlers (&first) != 0)
        abort ();
    struct sigaction ignored_pipe = { 0 };
    ignored_pipe.sa_handler = SIG_IGN;
    sigemptyset (&ignored_pipe.sa_mask);
    sigaddset (&ignored_pipe.sa_mask, SIGUSR1);
    if (sigaction (SIGPIPE, &ignored_pipe, NULL) != 0)
        abort ();
    remove_signal_handlers (&first);
    valid = signal_action_is (SIGPIPE, SIG_IGN, 1) && signal_session == NULL;
    tap_ok (tap, valid, "removal preserves a later SIGPIPE disposition with the same handler and a changed mask");
    if (sigaction (SIGPIPE, &app_action, NULL) != 0)
        abort ();

    for (unsigned failure_call = 1; failure_call <= 4; ++failure_call)
    {
        fail_signal_install_call = failure_call;
        signal_install_calls = 0;
        errno = 0;
        valid = install_signal_handlers (&first) == -1 && errno == EACCES && signal_session == NULL;
        for (unsigned index = 0; index < 4; ++index)
            valid &= signal_action_is (signal_numbers[index], record_app_signal, 1);
        valid &= install_signal_handlers (&second) == 0;
        remove_signal_handlers (&second);
        tap_ok (tap, valid, "failed signal installation rolls back every earlier disposition");
    }

    pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
    unsigned ready = 0;
    int start = 0;
    struct signal_registration_job jobs[2] = { { &first, &start_lock, &changed, &ready, &start, -1, 0 },
                                               { &second, &start_lock, &changed, &ready, &start, -1, 0 } };
    pthread_t threads[2];

    for (unsigned index = 0; index < 2; ++index)
        if (pthread_create (&threads[index], NULL, register_signal_owner, &jobs[index]) != 0)
            abort ();

    pthread_mutex_lock (&start_lock);
    while (ready != 2)
        pthread_cond_wait (&changed, &start_lock);
    start = 1;
    pthread_cond_broadcast (&changed);
    pthread_mutex_unlock (&start_lock);

    for (unsigned index = 0; index < 2; ++index)
        pthread_join (threads[index], NULL);

    valid = (jobs[0].result == 0 && jobs[1].result == -1 && jobs[1].error == EBUSY && signal_session == &first) ||
            (jobs[1].result == 0 && jobs[0].result == -1 && jobs[0].error == EBUSY && signal_session == &second);
    remove_signal_handlers (&first);
    remove_signal_handlers (&second);
    pthread_cond_destroy (&changed);
    pthread_mutex_destroy (&start_lock);
    tap_ok (tap, valid, "concurrent registrations elect exactly one signal owner");

    valid = signal_action_is (SIGTERM, record_app_signal, 1) && child_signal_delivery_matches (NULL, NULL, 1);
    tap_ok (tap, valid, "application signal handler resumes after the last session leaves");

    for (unsigned index = 0; index < 4; ++index)
        sigaction (signal_numbers[index], &original_actions[index], NULL);
}

#include "usfs_hidden_test.h"

static void record_native_diagnostic (void * data, const char * message)
{
    unsigned * calls = data;
    if (message == NULL || message[0] == '\0')
        abort ();
    ++*calls;
    errno = ERANGE;
}

static int reject_native_initialization (
    const struct usfs_client_request * request,
    const struct usfs_limits * limits,
    struct usfs_behavior * behavior
)
{
    (void)request;
    (void)limits;
    (void)behavior;
    return -ENOSPC;
}

static void test_native_api_contract (struct tap_state * tap)
{
    struct usfs_client sentinel = { 0 };
    struct usfs_client * client = &sentinel;
    struct usfs_operations operations = { 0 };
    int rc = usfs_client_create (USFS_CLIENT_API_VERSION + 1, &operations, sizeof (operations), NULL, NULL, &client);
    tap_ok (tap, rc == -EPROTONOSUPPORT && client == NULL, "unsupported native API version clears the output pointer");

    client = &sentinel;
    rc = usfs_client_create (USFS_CLIENT_API_VERSION, NULL, sizeof (operations), NULL, NULL, &client);
    tap_ok (tap, rc == -EINVAL && client == NULL, "absent native operations require a zero table size");

    rc = usfs_client_create (USFS_CLIENT_API_VERSION, &operations, sizeof (operations), NULL, NULL, NULL);
    tap_ok (tap, rc == -EINVAL, "native creation rejects an absent output pointer");

    for (unsigned invalid_option = 0; invalid_option < 3; ++invalid_option)
    {
        struct usfs_client_options options = { 0 };
        if (invalid_option == 0)
            options.mount_access = (enum usfs_mount_access) - 1;
        if (invalid_option == 1)
            options.behavior.remove_policy = (enum usfs_remove_policy) - 1;
        if (invalid_option == 2)
            options.behavior.handle_paths = (enum usfs_path_policy) - 1;
        client = &sentinel;
        rc = usfs_client_create (USFS_CLIENT_API_VERSION, &operations, sizeof (operations), &options, NULL, &client);
        tap_ok (tap, rc == -EINVAL && client == NULL, "invalid native option rejects creation and clears its output");
    }

    struct
    {
        struct usfs_operations known; // Current API prefix of a future callback table.
        uintptr_t extension;          // Unknown trailing storage ignored by this API version.
    } extended = { 0 };
    extended.known.getattr = hidden_getattr;
    rc = usfs_client_create (USFS_CLIENT_API_VERSION, &extended.known, sizeof (extended), NULL, NULL, &client);
    tap_ok (tap, rc == 0 && client != NULL && client->ops.getattr == hidden_getattr, "larger native operation tables preserve the known prefix");
    if (client == NULL)
        abort ();

    tap_ok (
        tap,
        usfs_client_run (client, 0) == -EINVAL && usfs_client_run (client, USFS_CLIENT_MAX_WORKERS + 1) == -EINVAL && !client->init_done,
        "invalid native worker counts fail before initialization"
    );
    rc = usfs_client_destroy (&client);
    tap_ok (tap, rc == 0 && client == NULL && usfs_client_destroy (&client) == 0, "native destruction consumes and clears the owning pointer");

    unsigned diagnostic_calls = 0;
    struct usfs_client_options options = { 0 };
    options.diagnostic = record_native_diagnostic;
    options.diagnostic_data = &diagnostic_calls;
    rc = usfs_client_create (USFS_CLIENT_API_VERSION, &operations, sizeof (operations), &options, NULL, &client);
    if (rc != 0)
        abort ();
    client->mounted = 1;
    errno = EACCES;
    rc = usfs_client_destroy (&client);
    tap_ok (
        tap,
        rc == -EBUSY && client != NULL && diagnostic_calls == 1 && errno == EACCES,
        "native diagnostic routing preserves errno and refused destruction preserves ownership"
    );
    client->mounted = 0;
    (void)usfs_client_destroy (&client);

    operations.initialize = reject_native_initialization;
    operations.shutdown = observe_destroy;
    destroy_calls = 0;
    client = new_test_client (&operations, sizeof (operations), NULL);
    if (client == NULL)
        abort ();
    usfs_client_request_stop (client);
    rc = usfs_client_run (client, 1);
    const int destroy_result = usfs_client_destroy (&client);
    tap_ok (
        tap,
        rc == -ENOSPC && destroy_result == 0 && client == NULL && destroy_calls == 0,
        "failed native initialization reports its error without a shutdown callback"
    );
}

int main (void)
{
    struct tap_state tap;
    tap_plan (&tap, 267);
    test_native_api_contract (&tap);
    tap_ok (&tap, get_callback_request () == NULL, "no callback context leaks outside callback scope");
    test_lifecycle_context (&tap);
    test_signal_ownership (&tap);
    /* Older backend-unit cases invoke callbacks directly. Supply their
     * explicit fixture context rather than relying on a process-global one. */
    struct usfs_client fixture = { 0 };
    struct client_context_scope fixture_scope __attribute__ ((cleanup (restore_callback_context))) = { 0 };
    if (enter_callback_context (&fixture, &fixture_scope) != 0)
        abort ();
    test_mount_ownership (&tap);
    test_mount_identity (&tap);
    test_unmount_retry (&tap);
    test_mount_access (&tap);
    test_statistics_reply (&tap);
    test_read_reply_bounds (&tap);
    test_readlink_termination (&tap);
    test_create_flags (&tap);
    test_namespace_transactions (&tap);
    test_detached_identity (&tap);
    test_detached_metadata (&tap);
    test_parent_identity (&tap);
    test_append_transactions (&tap);
    test_hardlink_identity (&tap);
    test_memfs_reservations (&tap);
    test_memfs_capacity (&tap);
    test_memfs_buffer_capacity (&tap);
    test_directory_failures (&tap);
    test_partial_readdir_error (&tap);
    test_directory_isolation (&tap);
    test_large_directory_spool (&tap);
    test_directory_ram_bound (&tap);
    test_atomic_create (&tap);
    test_identity_reclamation (&tap);
    test_fid_callback_pair (&tap);
    test_fid_vget_reconstruction (&tap);
    test_fid_detached_alias (&tap);
    test_hidden_configuration (&tap);
    test_hidden_lifetime (&tap);
    test_hard_remove_paths (&tap);
    test_hidden_failures (&tap);
    test_hidden_preparation_failures (&tap);
    test_null_handle_paths (&tap);
    test_nullpath_pathname_operation (&tap);
    test_hidden_concurrency (&tap);
    test_loop_failures (&tap);
    return tap_finish (&tap);
}

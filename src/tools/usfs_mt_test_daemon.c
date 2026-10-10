/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Dedicated E2E daemon proving that the native worker pool executes callbacks in
 * parallel. It is installed only with the testing build.
 */

#include "../../examples/usfs_example.h"
#include "usfs_file.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MT_CONTEXT_SLOT_COUNT               64
#define MT_LARGE_DIRECTORY_ENTRIES          140000u
#define NAMESPACE_RELEASE_TIMEOUT_SECONDS   30
#define NAMESPACE_RELEASE_POLL_MICROSECONDS 10000

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int active_reads;
static unsigned int maximum_reads;
static int context_collision;
static const struct usfs_client_request * active_contexts[MT_CONTEXT_SLOT_COUNT];
static const char contents[] = "parallel-dispatch\n";
static unsigned destroy_calls;
static int create_flags_mode;
static int namespace_mode, namespace_moved;
static unsigned barrier_readers;
static pthread_cond_t readers_changed = PTHREAD_COND_INITIALIZER;
static int wait_cleanup_command (const char * command_fifo_path);
static struct
{
    int exists;          // Whether the fixture has a namespace entry.
    int flags;           // Flags captured when the fixture was opened.
    int live;            // Whether its file handle is active.
    char value;          // Single stored byte of fixture data.
    size_t size;         // Logical fixture size in bytes.
    struct stat initial; // Attributes supplied when the fixture was created.
} created_files[3];

static int created_index (const char * path)
{
    if (strcmp (path, "/create-ro") == 0)
        return 0;
    if (strcmp (path, "/create-wo") == 0)
        return 1;
    if (strcmp (path, "/create-rw") == 0)
        return 2;
    return -1;
}

static int mt_create_flags (const struct usfs_client_request * request, const char * path, mode_t mode, struct usfs_open_file * file_info)
{
    (void)request;
    const int file_index = created_index (path);
    (void)mode;
    if (file_index < 0)
        return -ENOENT;
    if (created_files[file_index].exists)
        return -EEXIST;
    created_files[file_index].exists = created_files[file_index].live = 1;
    created_files[file_index].flags = file_info->open_flags;
    file_info->value = (uint64_t)file_index + 1;
    printf ("CREATE %d flags=%d fh=%llu\n", file_index, file_info->open_flags, (unsigned long long)file_info->value);
    fflush (stdout);
    return 0;
}

static int mt_create_attributes (
    const struct usfs_client_request * request,
    const char * path,
    const struct stat * initial,
    unsigned int valid,
    enum usfs_create_action activation,
    struct stat * attributes,
    struct usfs_open_file * file_info
)
{
    (void)request;
    const int file_index = created_index (path);
    int rc;
    if (file_index < 0)
        return -ENOENT;
    if (created_files[file_index].exists)
        return -EEXIST;
    if ((valid & USFS_INITIAL_SIZE) && (initial->st_size < 0 || initial->st_size > 1))
        return -EOPNOTSUPP;
    *attributes = *initial;
    attributes->st_ino = (ino_t)file_index + 10;
    attributes->st_nlink = 1;
    attributes->st_blksize = 4096;
    attributes->st_size = valid & USFS_INITIAL_SIZE ? initial->st_size : 0;
    /* Fixed fixture slots reserve both namespace and handle storage. */
    if (activation == USFS_CREATE_WITH_OPEN)
    {
        rc = mt_create_flags (request, path, initial->st_mode, file_info);
        if (rc != 0)
            return rc;
    }
    else
        created_files[file_index].exists = 1;
    created_files[file_index].initial = *attributes;
    created_files[file_index].size = (size_t)attributes->st_size;
    return 0;
}

static int mt_release_flags (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file_info)
{
    (void)request;
    const int file_index = created_index (path);
    const int valid = file_index >= 0 && file_info->value == (uint64_t)file_index + 1 && created_files[file_index].live &&
                      file_info->open_flags == created_files[file_index].flags;
    if (file_index >= 0)
        created_files[file_index].live = 0;
    printf ("RELEASE %d flags=%d fh=%llu valid=%d\n", file_index, file_info->open_flags, (unsigned long long)file_info->value, valid);
    fflush (stdout);
    return valid ? 0 : -EIO;
}

static ssize_t mt_write_flags (
    const struct usfs_client_request * request,
    const char * path,
    const char * data,
    size_t size,
    off_t offset,
    struct usfs_open_file * file_info
)
{
    (void)request;
    const int file_index = created_index (path);
    if (file_index < 0 || file_info->value != (uint64_t)file_index + 1 || !created_files[file_index].live)
        return -EBADF;
    if ((created_files[file_index].flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (offset != 0 || size != 1)
        return -EINVAL;
    created_files[file_index].value = data[0];
    created_files[file_index].size = 1;
    return 1;
}

static void mt_destroy (const struct usfs_client_request * request)
{
    (void)request;
    ++destroy_calls;
    if (getenv ("USFS_MT_STUCK_READ") != NULL || getenv ("USFS_MT_HELD_READ") != NULL || getenv ("USFS_MT_EXTERNAL_UNMOUNT") != NULL)
    {
        puts ("USFS_DESTROY_CALLED");
        fflush (stdout);
    }
}

static void record_maximum (void)
{
    const char * path = getenv ("USFS_MT_STATUS");
    FILE * stream;

    if (path == NULL || *path == '\0')
        return;
    stream = fopen (path, "w");
    if (stream == NULL)
        return;
    fprintf (stream, "max_active=%u\ncontext_collision=%d\n", maximum_reads, context_collision);
    fclose (stream);
}

static int read_namespace_attributes (const char * path, struct stat * attributes)
{
    int directory, file;
    pthread_mutex_lock (&state_lock);
    directory = strcmp (path, "/") == 0 || strcmp (path, namespace_moved ? "/new" : "/old") == 0;
    file = strcmp (path, namespace_moved ? "/new/file" : "/old/file") == 0;
    pthread_mutex_unlock (&state_lock);
    printf ("NAMESPACE_GETATTR %s valid=%d\n", path, directory || file);
    fflush (stdout);
    if (!directory && !file)
        return -ENOENT;
    attributes->st_mode = directory ? S_IFDIR | 0755 : S_IFREG | 0644;
    attributes->st_nlink = directory ? 2 : 1;
    attributes->st_ino = file ? 10 : strcmp (path, "/") == 0 ? 1 : 2;
    attributes->st_size = file ? sizeof (contents) - 1 : 0;
    return 0;
}

static int mt_getattr (const struct usfs_client_request * request, const char * path, struct stat * attributes, struct usfs_open_file * file_info)
{
    (void)request;
    (void)file_info;
    if (getenv ("USFS_MT_HELD_READ") != NULL)
    {
        puts ("USFS_GETATTR_ENTERED");
        fflush (stdout);
    }
    memset (attributes, 0, sizeof (*attributes));
    if (namespace_mode)
    {
        return read_namespace_attributes (path, attributes);
    }
    if (strcmp (path, "/") == 0)
    {
        attributes->st_mode = S_IFDIR | (create_flags_mode ? 0755 : 0555);
        attributes->st_nlink = 2;
        return 0;
    }
    if (create_flags_mode)
    {
        const int file_index = created_index (path);
        if (file_index >= 0 && created_files[file_index].exists)
        {
            if (created_files[file_index].initial.st_ino != 0)
                *attributes = created_files[file_index].initial;
            attributes->st_mode = S_IFREG | 0600;
            attributes->st_nlink = 1;
            attributes->st_ino = (ino_t)file_index + 10;
            attributes->st_size = (off_t)created_files[file_index].size;
            return 0;
        }
    }
    if (strcmp (path, "/slow") == 0 || strcmp (path, "/other") == 0)
    {
        attributes->st_mode = S_IFREG | 0444;
        attributes->st_nlink = 1;
        attributes->st_size = (off_t)(sizeof (contents) - 1);
        attributes->st_ino = strcmp (path, "/slow") == 0 ? 10 : 11;
        return 0;
    }
    return -ENOENT;
}

static int mt_readdir (
    const struct usfs_client_request * request,
    const char * path,
    const struct usfs_object_identity * identity,
    struct usfs_directory_sink * sink
)
{
    (void)request;
    (void)identity;
    if (strcmp (path, "/") != 0)
        return -ENOENT;

    if (getenv ("USFS_MT_LARGE_DIRECTORY") != NULL)
    {
        puts ("USFS_LARGE_READDIR_CALLBACK");
        fflush (stdout);

        if (usfs_directory_add (sink, ".", NULL) != 0)
            return 0;

        if (usfs_directory_add (sink, "..", NULL) != 0)
            return 0;

        for (unsigned entry_index = 0; entry_index < MT_LARGE_DIRECTORY_ENTRIES; ++entry_index)
        {
            char name[32];
            snprintf (name, sizeof (name), "entry-%06u", entry_index);
            if (usfs_directory_add (sink, name, NULL) != 0)
                break;
        }

        return 0;
    }

    usfs_directory_add (sink, ".", NULL);
    usfs_directory_add (sink, "..", NULL);
    usfs_directory_add (sink, "slow", NULL);
    return 0;
}

static int mt_open (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file_info)
{
    (void)request;
    if (namespace_mode)
    {
        if (strcmp (path, "/old/file") != 0)
            return -ENOENT;
        file_info->value = 10;
        return 0;
    }
    if (strcmp (path, "/slow") != 0 && strcmp (path, "/other") != 0)
        return -ENOENT;
    return (file_info->open_flags & O_ACCMODE) == O_RDONLY ? 0 : -EACCES;
}

static int mt_read_created_file (const char * path, char * buffer, size_t size, off_t offset, const struct usfs_open_file * file_info)
{
    const int file_index = created_index (path);

    if (file_index < 0 || file_info->value != (uint64_t)file_index + 1 || !created_files[file_index].live)
        return -EBADF;
    if ((created_files[file_index].flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (offset < 0)
        return -EINVAL;
    if ((size_t)offset >= created_files[file_index].size || size == 0)
        return 0;
    buffer[0] = created_files[file_index].value;
    return 1;
}

static int mt_begin_tracked_read (const struct usfs_client_request * request, unsigned int * context_slot)
{
    const struct usfs_client_request * context = request;
    unsigned int slot = MT_CONTEXT_SLOT_COUNT;
    unsigned int slot_index;

    pthread_mutex_lock (&state_lock);
    for (slot_index = 0; slot_index < MT_CONTEXT_SLOT_COUNT; slot_index++)
    {
        if (active_contexts[slot_index] == context)
            context_collision = 1;
        if (slot == MT_CONTEXT_SLOT_COUNT && active_contexts[slot_index] == NULL)
            slot = slot_index;
    }
    if (slot < MT_CONTEXT_SLOT_COUNT)
        active_contexts[slot] = context;
    active_reads += 1;
    if (active_reads > maximum_reads)
        maximum_reads = active_reads;
    record_maximum ();

    if (getenv ("USFS_MT_READ_BARRIER") != NULL)
    {
        struct timespec deadline;

        deadline.tv_sec = time (NULL) + 15;
        deadline.tv_nsec = 0;
        ++barrier_readers;
        pthread_cond_broadcast (&readers_changed);
        while (barrier_readers < 2)
        {
            if (pthread_cond_timedwait (&readers_changed, &state_lock, &deadline) != 0)
            {
                if (slot < MT_CONTEXT_SLOT_COUNT)
                    active_contexts[slot] = NULL;
                --active_reads;
                pthread_mutex_unlock (&state_lock);
                return -ETIMEDOUT;
            }
        }
    }
    pthread_mutex_unlock (&state_lock);
    *context_slot = slot;
    return 0;
}

static void mt_end_tracked_read (unsigned int context_slot)
{
    pthread_mutex_lock (&state_lock);
    if (context_slot < MT_CONTEXT_SLOT_COUNT)
        active_contexts[context_slot] = NULL;
    active_reads -= 1;
    pthread_mutex_unlock (&state_lock);
}

static void mt_apply_read_delay (void)
{
    const char * held_release = getenv ("USFS_MT_HELD_READ");

    if (held_release != NULL)
    {
        const time_t deadline = time (NULL) + 25;

        puts ("USFS_CALLBACK_HELD");
        fflush (stdout);
        while (access (held_release, F_OK) != 0 && time (NULL) < deadline)
            usleep (100000);
    }
    if (getenv ("USFS_MT_STUCK_READ") != NULL)
    {
        puts ("USFS_CALLBACK_STUCK");
        fflush (stdout);
        for (;;)
            sleep (1);
    }
    sleep (1);
}

static ssize_t mt_read (
    const struct usfs_client_request * request,
    const char * path,
    char * buffer,
    size_t size,
    off_t offset,
    struct usfs_open_file * file_info
)
{
    (void)request;
    size_t position;
    size_t available;
    unsigned int context_slot;
    int rc;

    if (create_flags_mode)
        return mt_read_created_file (path, buffer, size, offset, file_info);

    (void)file_info;
    if (strcmp (path, "/slow") != 0 && strcmp (path, "/other") != 0)
        return -ENOENT;
    if (offset < 0)
        return -EINVAL;
    position = (size_t)offset;
    if (position >= sizeof (contents) - 1)
        return 0;

    rc = mt_begin_tracked_read (request, &context_slot);
    if (rc != 0)
        return rc;
    mt_apply_read_delay ();

    available = sizeof (contents) - 1 - position;
    if (size > available)
        size = available;
    memcpy (buffer, contents + position, size);

    mt_end_tracked_read (context_slot);
    if (getenv ("USFS_MT_EXIT_FROM_READ") != NULL)
        usfs_client_request_stop (usfs_request_client (request));
    return (int)size;
}

static const struct usfs_operations mt_operations = {
    .getattr = mt_getattr,
    .readdir = mt_readdir,
    .open = mt_open,
    .read = mt_read,
    .shutdown = mt_destroy,
};

static int wait_namespace_release (const char * release_path)
{
    const time_t deadline = time (NULL) + NAMESPACE_RELEASE_TIMEOUT_SECONDS;

    while (access (release_path, F_OK) != 0)
    {
        if (time (NULL) >= deadline)
        {
            fprintf (stderr, "Failed to wait for namespace release %s: timed out\n", release_path);
            return -ETIMEDOUT;
        }

        usleep (NAMESPACE_RELEASE_POLL_MICROSECONDS);
    }

    return 0;
}

static int mt_namespace_rename (const struct usfs_client_request * request, const char * old_path, const char * new_path)
{
    (void)request;
    if (strcmp (old_path, "/old") || strcmp (new_path, "/new"))
        return -EINVAL;
    pthread_mutex_lock (&state_lock);
    namespace_moved = 1;
    pthread_mutex_unlock (&state_lock);
    puts ("USFS_RENAME_COMMITTED");
    fflush (stdout);
    return wait_namespace_release (getenv ("USFS_MT_NAMESPACE_RELEASE"));
}

static int wait_cleanup_command (const char * command_fifo_path)
{
    char byte;
    int file_descriptor = open (command_fifo_path, O_RDONLY);
    int result;
    if (file_descriptor < 0)
        return -1;
    result = read (file_descriptor, &byte, 1) == 1 ? 0 : -1;
    close (file_descriptor);
    return result;
}

static void * retry_serve (void * argument)
{
    (void)usfs_client_run (argument, 1);
    return NULL;
}

static int exercise_mount_retry (struct usfs_client * filesystem, const char * command_fifo_path)
{
    puts ("USFS_OWNER_READY");
    fflush (stdout);
    if (wait_cleanup_command (command_fifo_path) != 0)
        return 1;
    int rc = usfs_client_unmount (filesystem);
    if (rc != -EBUSY)
        return 1;
    rc = usfs_client_destroy (&filesystem);
    /* Never touch storage after an unexpected successful destruction. */
    if (destroy_calls != 0)
        _exit (1);
    if (rc != -EBUSY)
        return 1;
    puts ("USFS_RETRY_READY");
    fflush (stdout);
    if (wait_cleanup_command (command_fifo_path) != 0)
        return 1;
    return usfs_client_unmount (filesystem) == 0 ? 0 : 1;
}

static int mount_retry_probe (int argc, char ** argv, const char * command_fifo_path)
{
    struct example_arguments command_options = { 0 };
    struct usfs_client * filesystem = NULL;
    pthread_t worker;
    int started = 0, handlers = 0, result = 1;
    if (parse_example_arguments (&command_options, argc, argv) != 0 || command_options.mountpoint == NULL)
        goto out;
    (void)usfs_client_create (USFS_CLIENT_API_VERSION, &mt_operations, sizeof (mt_operations), &command_options.client, NULL, &filesystem);
    if (filesystem == NULL || usfs_client_mount (filesystem, command_options.mountpoint) != 0)
        goto out;
    if (usfs_client_install_signals (filesystem) != 0)
        goto out;
    handlers = 1;
    if (pthread_create (&worker, NULL, retry_serve, filesystem) != 0)
        goto out;
    started = 1;
    result = exercise_mount_retry (filesystem, command_fifo_path);
out:
    if (started)
    {
        pthread_kill (worker, SIGTERM);
        pthread_join (worker, NULL);
    }
    if (handlers)
        usfs_client_remove_signals (filesystem);
    if (filesystem != NULL)
    {
        if (usfs_client_unmount (filesystem) != 0)
            result = 1;
        if (usfs_client_destroy (&filesystem) != 0)
            result = 1;
    }
    if (result == 0 && destroy_calls == 1)
    {
        puts ("USFS_RETRY_DONE");
        return 0;
    }
    return 1;
}

/* Deterministic mount-owner journey: external removal ends the request loop,
 * then a FIFO keeps this instance alive until replacements have been mounted. */
static int mount_owner_probe (int argc, char ** argv, const char * command_fifo_path)
{
    struct example_arguments command_options = { 0 };
    struct usfs_client * filesystem = NULL;
    int result = 1, barrier = INVALID_FILE_DESCRIPTOR;
    char release;
    if (parse_example_arguments (&command_options, argc, argv) != 0)
        goto out;
    if (command_options.mountpoint == NULL)
        goto out;
    (void)usfs_client_create (USFS_CLIENT_API_VERSION, &mt_operations, sizeof (mt_operations), &command_options.client, NULL, &filesystem);
    if (filesystem == NULL || usfs_client_mount (filesystem, command_options.mountpoint) != 0)
        goto out;
    if (usfs_client_install_signals (filesystem) != 0)
        goto unmount;
    puts ("USFS_OWNER_READY");
    fflush (stdout);
    (void)usfs_client_run (filesystem, 1);
    usfs_client_remove_signals (filesystem);
    puts ("USFS_CLEANUP_READY");
    fflush (stdout);
    barrier = open (command_fifo_path, O_RDONLY);
    if (barrier >= 0 && read (barrier, &release, 1) == 1)
        result = 0;
unmount:
    (void)usfs_client_unmount (filesystem);
out:
    if (barrier >= 0)
        close (barrier);
    (void)usfs_client_destroy (&filesystem);
    return result;
}

/* The helper execs with the daemon's complete descriptor table. Neither its
 * inherited lifetime nor its exit may keep the transport or wake FIFO alive. */
static int exec_isolation_helper (void)
{
    struct stat transport;
    struct stat value;
    int file_descriptor;
    int limit = getdtablesize ();

    if (stat ("/dev/usfs0", &transport) != 0)
        return 1;
    for (file_descriptor = 3; file_descriptor < limit; ++file_descriptor)
    {
        if (fstat (file_descriptor, &value) == 0 && ((S_ISCHR (value.st_mode) && value.st_rdev == transport.st_rdev) || S_ISFIFO (value.st_mode)))
            return 1;
    }
    sleep (5);
    return 0;
}

struct idle_loop
{
    struct usfs_client * filesystem; // Filesystem served by the worker.
    int single;                      // Selects the single-threaded request loop.
    int result;                      // Request-loop return code collected after joining.
};

static void * idle_serve (void * argument)
{
    struct idle_loop * loop = argument;

    loop->result = usfs_client_run (loop->filesystem, loop->single ? 1 : USFS_CLIENT_DEFAULT_WORKERS);
    return NULL;
}

static pid_t start_exec_isolation_probe (const char * program)
{
    pid_t child = fork ();

    if (child == 0)
    {
        execl (program, program, "--exec-isolation-helper", (char *)NULL);
        _exit (127);
    }
    return child;
}

static int exec_isolation_probe_succeeded (pid_t child)
{
    int status;

    return waitpid (child, &status, 0) == child && WIFEXITED (status) && WEXITSTATUS (status) == 0;
}

static void stop_idle_worker (struct idle_loop * loop, pthread_t worker, unsigned int notifications)
{
    unsigned int slot_index;

    for (slot_index = 0; slot_index < notifications; ++slot_index)
        usfs_client_request_stop (loop->filesystem);
    pthread_join (worker, NULL);
}

static int destroy_idle_client (struct usfs_client * filesystem)
{
    int failed = 0;

    if (usfs_client_unmount (filesystem) != 0)
        failed = 1;
    if (usfs_client_destroy (&filesystem) != 0)
        failed = 1;
    return failed;
}

static int idle_exit_probe (int argc, char ** argv)
{
    struct example_arguments command_options = { 0 };
    struct idle_loop loop = { 0 };
    pthread_t worker;
    int worker_started = 0;
    int result = 1;
    pid_t child = -1;

    if (parse_example_arguments (&command_options, argc, argv) != 0 || command_options.mountpoint == NULL)
        goto out;
    loop.single = command_options.singlethread;
    (void)usfs_client_create (USFS_CLIENT_API_VERSION, &mt_operations, sizeof (mt_operations), &command_options.client, NULL, &loop.filesystem);
    if (loop.filesystem == NULL || usfs_client_mount (loop.filesystem, command_options.mountpoint) != 0)
        goto out;
    if (pthread_create (&worker, NULL, idle_serve, &loop) != 0)
        goto out;
    worker_started = 1;

    child = start_exec_isolation_probe (argv[0]);
    if (child < 0)
        goto out;
    sleep (2); /* Let the device reader enter its idle readiness wait. */
    stop_idle_worker (&loop, worker, 10000);
    worker_started = 0;
    if (loop.result != 0)
        goto out;
    result = 0;
out:
    if (worker_started)
        stop_idle_worker (&loop, worker, 1);
    if (loop.filesystem != NULL && destroy_idle_client (loop.filesystem) != 0)
        result = 1;
    if (child > 0 && !exec_isolation_probe_succeeded (child))
        result = 1;
    if (result == 0 && destroy_calls == 1)
    {
        puts ("USFS_IDLE_EXIT_AND_EXEC_ISOLATION_OK");
        return 0;
    }
    return 1;
}

int main (const int argc, char ** argv)
{
    if (argc == 2 && strcmp (argv[1], "--exec-isolation-helper") == 0)
    {
        return exec_isolation_helper ();
    }
    if (getenv ("USFS_MT_IDLE_EXIT") != NULL)
    {
        return idle_exit_probe (argc, argv);
    }

    const char * command_fifo_path = getenv ("USFS_MT_CLEANUP_FIFO");
    const char * retry = getenv ("USFS_MT_RETRY_FIFO");
    if (getenv ("USFS_MT_NAMESPACE_RELEASE") != NULL)
    {
        struct usfs_operations operations = mt_operations;
        namespace_mode = 1;
        operations.rename = mt_namespace_rename;
        struct example_arguments arguments = { 0 };
        if (parse_example_arguments (&arguments, argc, argv) != 0)
            return EXIT_FAILURE;
        return run_example_client (&arguments, &operations);
    }
    if (retry != NULL)
        return mount_retry_probe (argc, argv, retry);
    if (command_fifo_path != NULL)
        return mount_owner_probe (argc, argv, command_fifo_path);
    if (getenv ("USFS_MT_CREATE_FLAGS") != NULL)
    {
        struct usfs_operations operations = mt_operations;
        create_flags_mode = 1;
        operations.create = mt_create_flags;
        operations.create_attr = mt_create_attributes;
        operations.write = mt_write_flags;
        operations.release = mt_release_flags;
        struct example_arguments arguments = { 0 };
        if (parse_example_arguments (&arguments, argc, argv) != 0)
            return EXIT_FAILURE;
        return run_example_client (&arguments, &operations);
    }
    struct example_arguments arguments = { 0 };
    if (parse_example_arguments (&arguments, argc, argv) != 0)
        return EXIT_FAILURE;
    return run_example_client (&arguments, &mt_operations);
}

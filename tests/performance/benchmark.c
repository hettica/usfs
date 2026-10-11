/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 * Backend-neutral USFS calibration. Run only on the AIX host.
 */
#define _ALL_SOURCE
#include <dirent.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syncvfs.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <utime.h>

#define main overhead_memfs_example_main
#include "../../examples/memfs.c"
#undef main

#include "usfs_proto.h"
#include "usfs_status.h"
#include "usfs_test.h"
#include "timing.h"
#include "callback_measurements.h"

struct server_response
{
    int result;               // Zero or a negative client error.
    struct rusage usage;      // All daemon threads' process CPU at this boundary.
    uint64_t allocated_bytes; // Backend's explicitly retained file buffers.
    uint64_t inode_count;     // Backend's live namespace object count.
};

struct overhead_server
{
    pid_t pid;               // Owned calibration daemon.
    int command;             // Parent-to-daemon control pipe.
    int response;            // Daemon-to-parent control pipe.
    const char * mountpoint; // Owned existing empty mount directory.
};

static void interrupt_overhead (const int signal_number)
{
    (void)signal_number;
    overhead_interrupted = 1;
}

static int retained_status_descriptor = -1;

static int transfer_control_bytes (const int descriptor, void * buffer, const size_t count, const bool sending)
{
    size_t completed = 0;
    while (completed < count)
    {
        if (!sending)
        {
            struct pollfd response = { .fd = descriptor, .events = POLLIN };
            const int ready = poll (&response, 1, OVERHEAD_CONTROL_TIMEOUT_MS);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready <= 0)
            {
                errno = ready == 0 ? ETIMEDOUT : errno;
                return -1;
            }
        }
        const ssize_t transferred = sending ? write (descriptor, (char *)buffer + completed, count - completed)
                                            : read (descriptor, (char *)buffer + completed, count - completed);
        if (transferred < 0 && errno == EINTR)
            continue;
        if (transferred <= 0)
            return -1;
        completed += (size_t)transferred;
    }
    return 0;
}

static int fill_calibration_backend (void)
{
    options.size_mb = OVERHEAD_BACKEND_MIB;
    options.inode_limit = OVERHEAD_BACKEND_INODES;
    if (initialize_memory_filesystem () != 0)
        return -1;

    for (unsigned index = 0; index < OVERHEAD_CLIENTS; ++index)
    {
        char path[OVERHEAD_NAME_CAPACITY];
        snprintf (path, sizeof (path), "/data-%u", index);
        struct usfs_open_file file_info = { .open_flags = O_RDWR };
        int rc = memfs_create (NULL, path, OVERHEAD_FILE_MODE, &file_info);
        if (rc != 0)
            return rc;
        rc = memfs_truncate (NULL, path, OVERHEAD_FILE_BYTES, &file_info);
        if (rc != 0)
            return rc;
        struct mem_inode * inode = find_operation_inode (NULL, path, &file_info);
        if (inode == NULL)
            return -EIO;
        if (overhead_shared->application_uid != 0)
        {
            inode->uid = overhead_shared->application_uid;
            inode->gid = overhead_shared->application_uid;
        }
        memset (inode->data, OVERHEAD_READ_BYTE, inode->size);
        rc = memfs_release (NULL, path, &file_info);
        if (rc != 0)
            return rc;
    }
    return 0;
}

static void * run_calibration_client (void * argument)
{
    struct usfs_client * client = argument;
    const int rc = usfs_client_run (client, overhead_shared->workers);
    return (void *)(intptr_t)rc;
}

static int send_server_response (const int descriptor, const int result)
{
    struct server_response response = { .result = result, .allocated_bytes = g_allocated, .inode_count = g_inode_count };
    if (getrusage (RUSAGE_SELF, &response.usage) != 0)
        return -1;
    return transfer_control_bytes (descriptor, &response, sizeof (response), true);
}

static int serve_calibration_commands (
    const int command_descriptor,
    const int response_descriptor,
    const char * mountpoint,
    const bool fallback_creation
)
{
    int rc = fill_calibration_backend ();
    if (send_server_response (response_descriptor, rc) != 0 || rc != 0)
        return -1;

    struct usfs_client * client = NULL;
    pthread_t client_thread;
    bool running = false;
    struct usfs_operations operations_table = measured_operations;
    if (fallback_creation)
        operations_table.create_attr = NULL;

    for (;;)
    {
        char command;
        ssize_t command_bytes;
        do
        {
            command_bytes = read (command_descriptor, &command, sizeof (command));
        }
        while (command_bytes < 0 && errno == EINTR);
        if (command_bytes != sizeof (command))
            break;
        if (command == 'M')
        {
            rc = usfs_client_create (USFS_CLIENT_API_VERSION, &operations_table, sizeof (operations_table), NULL, NULL, &client);
            if (rc == 0)
                rc = usfs_client_mount (client, mountpoint);
            if (rc == 0)
                rc = usfs_client_install_signals (client);
            if (rc == 0)
            {
                rc = pthread_create (&client_thread, NULL, run_calibration_client, client);
                if (rc > 0)
                    rc = -rc;
                running = rc == 0;
            }
            if (send_server_response (response_descriptor, rc) != 0 || rc != 0)
                break;
            continue;
        }
        if (command == 'B' || command == 'E')
        {
            if (send_server_response (response_descriptor, 0) != 0)
                break;
            continue;
        }
        if (command == 'Q')
            break;
        rc = -EINVAL;
        break;
    }

    if (running)
    {
        void * thread_result = NULL;
        usfs_client_request_stop (client);
        if (pthread_join (client_thread, &thread_result) != 0)
            rc = -EIO;
        else if (rc == 0)
            rc = (int)(intptr_t)thread_result;
    }
    if (client != NULL)
    {
        usfs_client_remove_signals (client);
        const int unmount_result = usfs_client_unmount (client);
        if (rc == 0)
            rc = unmount_result;
        const int destroy_result = usfs_client_destroy (&client);
        if (rc == 0)
            rc = destroy_result;
    }
    (void)send_server_response (response_descriptor, rc);
    return rc;
}

static int request_server_snapshot (const struct overhead_server * server, const char command, struct server_response * response)
{
    char request = command;
    if (transfer_control_bytes (server->command, &request, sizeof (request), true) != 0)
        return -1;
    if (transfer_control_bytes (server->response, response, sizeof (*response), false) != 0)
        return -1;
    return response->result;
}

static void log_lifecycle_callbacks (const char * phase)
{
    for (unsigned kind = 0; kind < CALLBACK_COUNT; ++kind)
    {
        const struct callback_metric metric = read_callback_metric (kind);
        if (metric.calls == 0)
            continue;
        fprintf (
            stderr,
            "lifecycle-callback,%s,%s,%llu,%llu,%llu\n",
            phase,
            callback_names[kind],
            (unsigned long long)metric.calls,
            (unsigned long long)metric.wall_ns,
            (unsigned long long)metric.cpu_ns
        );
    }
}

static int start_overhead_server (struct overhead_server * server, const bool fallback_creation)
{
    int commands[2], responses[2];
    if (pipe (commands) != 0 || pipe (responses) != 0)
        return -1;
    server->pid = fork ();
    if (server->pid < 0)
        return -1;
    if (server->pid == 0)
    {
        close (commands[1]);
        close (responses[0]);
        const int rc = serve_calibration_commands (commands[0], responses[1], server->mountpoint, fallback_creation);
        close (commands[0]);
        close (responses[1]);
        _exit (rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    close (commands[0]);
    close (responses[1]);
    server->command = commands[1];
    server->response = responses[0];

    struct server_response response = { 0 };
    if (transfer_control_bytes (server->response, &response, sizeof (response), false) != 0)
        return -1;
    if (response.result != 0)
        return response.result;
    reset_callback_metrics ();
    __atomic_store_n (&overhead_shared->enabled, 1, __ATOMIC_RELEASE);
    const uint64_t started_ns = monotonic_ns ();
    int rc = request_server_snapshot (server, 'M', &response);
    /* A successful native lookup confirms initialization and the worker loop
     * are ready before measuring steady state or resetting callback totals. */
    struct stat attributes = { 0 };
    if (rc == 0)
        rc = stat (server->mountpoint, &attributes);
    const uint64_t elapsed_ns = monotonic_ns () - started_ns;
    __atomic_store_n (&overhead_shared->enabled, 0, __ATOMIC_RELEASE);
    fprintf (
        stderr,
        "lifecycle,mount,%llu,backend_allocated=%llu,daemon_maxrss_kib=%ld\n",
        (unsigned long long)elapsed_ns,
        (unsigned long long)response.allocated_bytes,
        response.usage.ru_maxrss
    );
    log_lifecycle_callbacks ("mount");
    return rc;
}

static int stop_overhead_server (struct overhead_server * server)
{
    struct server_response response = { 0 };
    reset_callback_metrics ();
    __atomic_store_n (&overhead_shared->enabled, 1, __ATOMIC_RELEASE);
    const uint64_t started_ns = monotonic_ns ();
    int rc = request_server_snapshot (server, 'Q', &response);
    const uint64_t elapsed_ns = monotonic_ns () - started_ns;
    __atomic_store_n (&overhead_shared->enabled, 0, __ATOMIC_RELEASE);
    fprintf (
        stderr,
        "lifecycle,unmount,%llu,backend_allocated=%llu,daemon_maxrss_kib=%ld\n",
        (unsigned long long)elapsed_ns,
        (unsigned long long)response.allocated_bytes,
        response.usage.ru_maxrss
    );
    log_lifecycle_callbacks ("unmount");
    close (server->command);
    close (server->response);
    int status;
    if (waitpid (server->pid, &status, 0) != server->pid)
        rc = -1;
    else if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
        rc = -1;
    return rc;
}

static int capture_kernel_status (struct kext_state * status)
{
    if (retained_status_descriptor >= 0)
        return ioctl (retained_status_descriptor, USFS_IOC_GET_KEXT_STATE, status);

    const int descriptor = open ("/dev/usfs0", O_RDWR);
    if (descriptor < 0)
        return -1;
    const int rc = ioctl (descriptor, USFS_IOC_GET_KEXT_STATE, status);
    const int close_result = close (descriptor);
    if (rc != 0)
        return rc;
    return close_result;
}

static int select_application_identity (void)
{
    if (overhead_shared->application_uid == 0)
        return 0;

    /* Keep the privileged status descriptor in this coordinator; application
     * I/O still executes after dropping all IDs and supplementary groups. */
    retained_status_descriptor = open ("/dev/usfs0", O_RDWR);
    if (retained_status_descriptor < 0)
        return -1;
    if (setgroups (0, NULL) != 0)
        return -1;
    if (setgid (overhead_shared->application_uid) != 0)
        return -1;
    if (setuid (overhead_shared->application_uid) != 0)
        return -1;

    fprintf (
        stderr,
        "application-identity,uid=%lu,euid=%lu,gid=%lu\n",
        (unsigned long)getuid (),
        (unsigned long)geteuid (),
        (unsigned long)getgid ()
    );
    return 0;
}

static void write_callback_metrics (FILE * output, const char * operation, const unsigned round)
{
    for (unsigned kind = 0; kind < CALLBACK_COUNT; ++kind)
    {
        const struct callback_metric metric = read_callback_metric (kind);
        if (metric.calls == 0)
            continue;
        fprintf (
            output,
            "%s,%u,%s,%llu,%llu,%llu,%llu,%llu\n",
            operation,
            round,
            callback_names[kind],
            (unsigned long long)metric.calls,
            (unsigned long long)metric.wall_ns,
            (unsigned long long)metric.cpu_ns,
            (unsigned long long)metric.bytes,
            (unsigned long long)metric.errors
        );
    }
    fflush (output);
}

#include "filesystem_operations.h"

static int execute_overhead_command (const int argc, char ** argv)
{
    if (argc != 9)
    {
        fprintf (
            stderr,
            "Usage: overhead mountpoint output-prefix pool delay-us instrumented fallback rounds metadata|data|smoke|data-smoke|identifiers\n"
        );
        return -1;
    }
    unsigned long pool, delay, instrumented, fallback, rounds;
    if (parse_example_number (argv[3], USFS_CLIENT_MAX_WORKERS, &pool) != 0)
        return -1;
    /* Zero is meaningful for delays and optional measurement modes. */
    char * end;
    errno = 0;
    delay = strtoul (argv[4], &end, 10);
    if (errno != 0 || end == argv[4] || *end != '\0' || delay > OVERHEAD_MICROSECONDS)
        return -1;
    instrumented = strtoul (argv[5], &end, 10);
    if (errno != 0 || end == argv[5] || *end != '\0' || instrumented > 1)
        return -1;
    fallback = strtoul (argv[6], &end, 10);
    if (errno != 0 || end == argv[6] || *end != '\0' || fallback > 1)
        return -1;
    if (parse_example_number (argv[7], OVERHEAD_MAX_ROUNDS, &rounds) != 0)
        return -1;
    if (strcmp (argv[8], "metadata") != 0 && strcmp (argv[8], "data") != 0 && strcmp (argv[8], "smoke") != 0 && strcmp (argv[8], "data-smoke") != 0 &&
        strcmp (argv[8], "identifiers") != 0)
        return -1;
    overhead_shared = mmap (NULL, sizeof (*overhead_shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (overhead_shared == MAP_FAILED)
        return -1;
    memset (overhead_shared, 0, sizeof (*overhead_shared));
    overhead_shared->workers = (unsigned)pool;
    overhead_shared->delay_us = (unsigned)delay;
    overhead_shared->instrumented = (unsigned)instrumented;
    const char * application_uid = getenv ("USFS_OVERHEAD_UID");
    if (application_uid != NULL)
    {
        if (strcmp (argv[8], "data") != 0 && strcmp (argv[8], "data-smoke") != 0)
        {
            fprintf (stderr, "Failed to select application identity: use data or data-smoke suite\n");
            munmap (overhead_shared, sizeof (*overhead_shared));
            return -1;
        }
        unsigned long parsed_uid;
        if (parse_example_number (application_uid, INT_MAX, &parsed_uid) != 0)
        {
            munmap (overhead_shared, sizeof (*overhead_shared));
            return -1;
        }
        overhead_shared->application_uid = (uid_t)parsed_uid;
    }
    struct overhead_server server = { .pid = -1, .mountpoint = argv[1] };
    int rc = start_overhead_server (&server, fallback != 0);
    if (rc == 0)
        rc = run_overhead_workloads (&server, argv[2], (unsigned)rounds, argv[8], fallback != 0);
    const int stop_result = server.pid > 0 ? stop_overhead_server (&server) : -1;
    if (rc == 0)
        rc = stop_result;
    if (retained_status_descriptor >= 0)
        if (close (retained_status_descriptor) != 0)
            rc = -1;
    munmap (overhead_shared, sizeof (*overhead_shared));
    return rc;
}

int main (const int argc, char ** argv)
{
    signal (SIGPIPE, SIG_IGN);
    signal (SIGTERM, interrupt_overhead);
    signal (SIGINT, interrupt_overhead);
    const int rc = execute_overhead_command (argc, argv);
    if (rc != 0)
        fprintf (stderr, "Failed to complete overhead campaign: result=%d errno=%d (%s)\n", rc, errno, strerror (errno));
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

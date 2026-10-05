/*
    Copyright (c) 2026 Raman Dzehtsiar
    SPDX-License-Identifier: MIT

    usfsctl executable - USFS administration and diagnostics utility.

    Invoked directly by administrators or scripts to inspect the running
    filesystem and collect diagnostic information during normal operation
    or troubleshooting. It provides device status reporting and manages
    native AIX trace sessions, with readable reports for later analysis.
 */

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/procfs.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

#include "usfs_common.h"
#include "usfs_config.h"
#include "usfs_proto.h"
#include "usfs_status.h"

#define USFSCTL_UNAVAILABLE  2
#define USFSCTL_WRONG_USAGE  3
#define USFSCTL_TRACE_STATE  "/var/adm/ras/usfs.trace.state"
#define USFSCTL_TRACE_FORMAT "/usr/lib/ras/usfs.trcfmt"

#ifndef USFS_BUILD_REVISION
    #define USFS_BUILD_REVISION "unknown"
#endif

#ifndef USFS_PACKAGE_VERSION
    #define USFS_PACKAGE_VERSION "unknown"
#endif

static const char * status_device_file = "/dev/usfs0/status";

struct usfsctl_trace_state
{
    char trace_profile_name[16];                            // Selected trace profile: core, requests, or full.
    char trace_raw_output_path[4096];                       // Absolute path to the raw trace output file.
    unsigned long long trace_raw_output_device_id;          // Device containing the trace file, used to verify its identity.
    unsigned long long trace_raw_output_inode;              // Trace file inode, used with the device ID to verify its identity.
    long trace_daemon_process_id;                           // Process ID of the AIX trace daemon owning the session.
    unsigned long long trace_daemon_start_time_seconds;     // Seconds component of the daemon start time, used to detect PID reuse.
    unsigned long long trace_daemon_start_time_nanoseconds; // Nanoseconds component of the same daemon start time.
};

static int read_process_info (const pid_t process_id, psinfo_t * process_info)
{
    char process_info_path[64] = { 0 };

    if (snprintf (process_info_path, sizeof (process_info_path), "/proc/%ld/psinfo", (long)process_id) >= (int)sizeof (process_info_path))
        return -1;

    const int process_info_descriptor = open (process_info_path, O_RDONLY);

    if (process_info_descriptor < 0)
        return -1;

    const ssize_t bytes_read = read (process_info_descriptor, process_info, sizeof (*process_info));
    (void)close (process_info_descriptor);

    return bytes_read == (ssize_t)sizeof (*process_info) ? 0 : -1;
}

static int is_trace_daemon (const psinfo_t * process_info)
{
    if (process_info->pr_euid != 0)
        return false;

    if (strcmp (process_info->pr_fname, "trace") != 0)
        return false;

    if (strstr (process_info->pr_psargs, "/usr/bin/trace -a -d") == NULL)
        return false;

    return true;
}

static int read_matching_trace_daemon (const char * process_directory_name, const time_t earliest_start_time, psinfo_t * process_info)
{
    char * unparsed_suffix = NULL;
    const long process_id = strtol (process_directory_name, &unparsed_suffix, 10);

    if (process_directory_name[0] == '\0')
        return false;

    if (unparsed_suffix == NULL)
        return false;

    if (*unparsed_suffix != '\0')
        return false;

    if (process_id <= 0)
        return false;

    if (process_id > INT_MAX)
        return false;

    if (read_process_info ((pid_t)process_id, process_info) != 0)
        return false;

    if (!is_trace_daemon (process_info))
        return false;

    if (process_info->pr_start.tv_sec < earliest_start_time)
        return false;

    return true;
}

static int find_trace_daemon (const time_t earliest_start_time, psinfo_t * trace_daemon_process_info)
{
    DIR * process_directory = opendir ("/proc");

    if (process_directory == NULL)
    {
        fprintf (stderr, "Failed to find trace daemon: could not open process directory /proc: %s\n", strerror (errno));
        return -1;
    }

    const struct dirent * process_directory_entry;
    int matching_process_count = 0;

    while ((process_directory_entry = readdir (process_directory)) != NULL)
    {
        psinfo_t process_info;

        if (!read_matching_trace_daemon (process_directory_entry->d_name, earliest_start_time, &process_info))
            continue;

        *trace_daemon_process_info = process_info;
        matching_process_count += 1;
    }

    (void)closedir (process_directory);

    if (matching_process_count != 1)
    {
        fprintf (stderr, "Failed to identify trace daemon: found %d matching processes started at or after %lld seconds since the epoch\n", matching_process_count, (long long)earliest_start_time);
        return -1;
    }

    return 0;
}

static void redirect_command_output (const char * output_file_path)
{
    if (output_file_path == NULL)
        return;

    const int output_file_descriptor = open (output_file_path, O_WRONLY | O_CREAT | O_EXCL, 0600);

    if (output_file_descriptor < 0)
    {
        fprintf (stderr, "Failed to create output file %s: %s\n", output_file_path, strerror (errno));
        _exit (126);
    }

    if (dup2 (output_file_descriptor, STDOUT_FILENO) < 0)
    {
        fprintf (stderr, "Failed to redirect output to file %s: %s\n", output_file_path, strerror (errno));
        _exit (126);
    }

    (void)close (output_file_descriptor);
}

static int wait_for_command (const pid_t child_process_id, const char * action_description)
{
    int child_process_status;

    do
    {
        if (waitpid (child_process_id, &child_process_status, 0) < 0)
        {
            fprintf (stderr, "Failed to %s: could not wait for process %ld: %s\n", action_description, (long)child_process_id, strerror (errno));
            return -1;
        }
    }
    while (!WIFEXITED (child_process_status) && !WIFSIGNALED (child_process_status));

    if (!WIFEXITED (child_process_status))
    {
        fprintf (stderr, "Failed to %s: process %ld terminated by signal %d\n", action_description, (long)child_process_id, WTERMSIG (child_process_status));
        return -1;
    }

    const int rc = WEXITSTATUS (child_process_status);

    if (rc != 0)
        fprintf (stderr, "Failed to %s: process %ld exited with status %d\n", action_description, (long)child_process_id, rc);

    return rc;
}

static int run_command (const char * const argv[], const char * output_file_path, const char * action_description)
{
    const pid_t child_process_id = fork ();

    if (child_process_id < 0)
    {
        fprintf (stderr, "Failed to %s: could not create child process: %s\n", action_description, strerror (errno));
        return -1;
    }

    if (child_process_id == 0)
    {
        redirect_command_output (output_file_path);
        execv (argv[0], (char * const *)argv);
        fprintf (stderr, "Failed to %s: could not execute child process: %s\n", action_description, strerror (errno));
        _exit (127);
    }

    return wait_for_command (child_process_id, action_description);
}

static int is_valid_trace_path (const char * path)
{
    if (path == NULL)
        return false;

    if (path[0] != '/')
        return false;

    if (strlen (path) >= PATH_MAX)
        return false;

    for (const unsigned char * path_character = (const unsigned char *)path; *path_character != '\0'; ++path_character)
    {
        switch (*path_character)
        {
            case ' ':
            case '\t':
            case '\n':
            case '\r':
                return false;
            default:;
        }
    }

    return true;
}

static int read_trace_state (struct usfsctl_trace_state * state)
{
    FILE * state_stream = fopen (USFSCTL_TRACE_STATE, "r");

    if (state_stream == NULL)
        return -1;

    memset (state, 0, sizeof (*state));
    const int fields_read = fscanf (state_stream, "%15s %4095s %llu %llu %ld %llu %llu", state->trace_profile_name, state->trace_raw_output_path, &state->trace_raw_output_device_id, &state->trace_raw_output_inode, &state->trace_daemon_process_id, &state->trace_daemon_start_time_seconds, &state->trace_daemon_start_time_nanoseconds);
    (void)fclose (state_stream);

    if (fields_read != 7)
        return -1;

    if (state->trace_daemon_process_id <= 0)
        return -1;

    if (!is_valid_trace_path (state->trace_raw_output_path))
        return -1;

    return 0;
}

static int read_and_verify_trace_session (struct usfsctl_trace_state * state)
{
    if (read_trace_state (state) != 0)
        return false;

    struct stat raw_trace_file_metadata;

    if (stat (state->trace_raw_output_path, &raw_trace_file_metadata) != 0)
        return false;

    psinfo_t trace_daemon_process_info;

    if (read_process_info ((pid_t)state->trace_daemon_process_id, &trace_daemon_process_info) != 0)
        return false;

    if (!is_trace_daemon (&trace_daemon_process_info))
        return false;

    if (state->trace_raw_output_device_id != (unsigned long long)raw_trace_file_metadata.st_dev)
        return false;

    if (state->trace_raw_output_inode != (unsigned long long)raw_trace_file_metadata.st_ino)
        return false;

    if (state->trace_daemon_start_time_seconds != (unsigned long long)trace_daemon_process_info.pr_start.tv_sec)
        return false;

    if (state->trace_daemon_start_time_nanoseconds != (unsigned long long)trace_daemon_process_info.pr_start.tv_nsec)
        return false;

    return true;
}

static void remove_trace_state_file (void)
{
    // todo: check project for ignored return codes; each case must be explained
    if (unlink (USFSCTL_TRACE_STATE) != 0)
        fprintf (stderr, "Failed to remove trace ownership state file %s: %s\n", USFSCTL_TRACE_STATE, strerror (errno));
}

static int write_trace_state (const char * trace_profile_name, const char * raw_trace_output_path, const struct stat * raw_trace_file_metadata, const psinfo_t * trace_daemon_process_info)
{
    const int state_file_descriptor = open (USFSCTL_TRACE_STATE, O_WRONLY | O_CREAT | O_EXCL, 0600);

    if (state_file_descriptor < 0)
    {
        fprintf (stderr, "Failed to create trace ownership state file %s: %s\n", USFSCTL_TRACE_STATE, strerror (errno));
        return -1;
    }

    FILE * state_stream = fdopen (state_file_descriptor, "w");

    if (state_stream == NULL)
    {
        fprintf (stderr, "Failed to open trace ownership state stream for %s: %s\n", USFSCTL_TRACE_STATE, strerror (errno));
        (void)close (state_file_descriptor);
        remove_trace_state_file ();
        return -1;
    }

    if (fprintf (state_stream, "%s %s %llu %llu %ld %llu %llu\n", trace_profile_name, raw_trace_output_path, (unsigned long long)raw_trace_file_metadata->st_dev, (unsigned long long)raw_trace_file_metadata->st_ino, (long)trace_daemon_process_info->pr_pid, (unsigned long long)trace_daemon_process_info->pr_start.tv_sec, (unsigned long long)trace_daemon_process_info->pr_start.tv_nsec) < 0)
    {
        fprintf (stderr, "Failed to write trace ownership state file %s: %s\n", USFSCTL_TRACE_STATE, strerror (errno));
        remove_trace_state_file ();
        return -1;
    }

    if (fclose (state_stream) != 0)
    {
        fprintf (stderr, "Failed to close trace ownership state file %s: %s\n", USFSCTL_TRACE_STATE, strerror (errno));
        remove_trace_state_file ();
        return -1;
    }

    return 0;
}

static const char * get_trace_hooks (const char * trace_profile_name)
{
    if (strcmp (trace_profile_name, "core") == 0)
        return "F5F1";

    if (strcmp (trace_profile_name, "requests") == 0)
        return "F5F1,F5F2";

    if (strcmp (trace_profile_name, "full") == 0)
        return "F5F1,F5F2,F5F3";

    return NULL;
}

static int make_default_trace_path (char * raw_trace_output_path, const size_t path_capacity)
{
    const time_t current_time = time (NULL);
    struct tm utc_time;

    if (gmtime_r (&current_time, &utc_time) == NULL)
        return -1;

    char utc_timestamp[32];

    if (strftime (utc_timestamp, sizeof (utc_timestamp), "%Y%m%dT%H%M%SZ", &utc_time) == 0)
        return -1;

    return snprintf (raw_trace_output_path, path_capacity, "/var/adm/ras/usfs-%s-%ld.trc", utc_timestamp, (long)getpid ()) < (int)path_capacity ? 0 : -1;
}

struct usfsctl_trace_start_options
{
    const char * trace_profile_name;    // Trace profile requested by the user, defaulting to requests.
    const char * raw_trace_output_path; // Requested raw trace file path, or NULL to generate one.
};

static int parse_trace_start_option (const char * option_name, const char * option_value, struct usfsctl_trace_start_options * options)
{
    if (strcmp (option_name, "--profile") == 0)
    {
        options->trace_profile_name = option_value;
        return 0;
    }

    if (strcmp (option_name, "--output") == 0)
    {
        options->raw_trace_output_path = option_value;
        return 0;
    }

    return USFSCTL_WRONG_USAGE;
}

static int parse_trace_start_options (const int argc, char ** argv, struct usfsctl_trace_start_options * options)
{
    options->trace_profile_name = "requests";
    options->raw_trace_output_path = NULL;

    for (int argument_index = 0; argument_index < argc; argument_index += 2)
    {
        if (argument_index + 1 >= argc)
            return USFSCTL_WRONG_USAGE;

        const char * option_name = argv[argument_index];
        const char * option_value = argv[argument_index + 1];

        if (parse_trace_start_option (option_name, option_value, options) != 0)
            return USFSCTL_WRONG_USAGE;
    }

    return 0;
}

static int write_trace_report (const char * raw_trace_output_path, const char * requested_report_path)
{
    if (!is_valid_trace_path (raw_trace_output_path))
    {
        fprintf (stderr, "Failed to format trace report: invalid raw trace path %s\n", raw_trace_output_path == NULL ? "(missing)" : raw_trace_output_path);
        return USFSCTL_WRONG_USAGE;
    }

    char generated_report_path[PATH_MAX];
    const char * formatted_trace_report_path = requested_report_path;

    if (formatted_trace_report_path == NULL)
    {
        if (snprintf (generated_report_path, sizeof (generated_report_path), "%s.txt", raw_trace_output_path) >= (int)sizeof (generated_report_path))
        {
            fprintf (stderr, "Failed to format trace report path for %s: path exceeds %lu bytes\n", raw_trace_output_path, (unsigned long)sizeof (generated_report_path) - 1);
            return USFSCTL_WRONG_USAGE;
        }

        formatted_trace_report_path = generated_report_path;
    }

    if (!is_valid_trace_path (formatted_trace_report_path))
    {
        fprintf (stderr, "Failed to create formatted trace report %s: invalid path\n", formatted_trace_report_path);
        return USFSCTL_UNAVAILABLE;
    }

    if (access (formatted_trace_report_path, F_OK) == 0)
    {
        fprintf (stderr, "Failed to create formatted trace report %s: file already exists\n", formatted_trace_report_path);
        return USFSCTL_UNAVAILABLE;
    }

    const char * report_command_args[] = {
        "/usr/bin/trcrpt",
        "-t",
        USFSCTL_TRACE_FORMAT,
        "-O",
        "timestamp=1,pid=on,tid=on,cpuid=on",
        raw_trace_output_path,
        NULL
    };

    if (run_command (report_command_args, formatted_trace_report_path, "format trace report") != 0)
    {
        fprintf (stderr, "Failed to format trace report %s: raw trace retained at %s\n", formatted_trace_report_path, raw_trace_output_path);
        return USFSCTL_UNAVAILABLE;
    }

    printf ("Created formatted trace report: path=%s\n", formatted_trace_report_path);
    return 0;
}

static uint64_t add_saturating (const uint64_t left, const uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static struct usfs_latency_stats combine_latency_stats (const struct kext_state * state)
{
    struct usfs_latency_stats combined_latency = { 0 };
    combined_latency.samples = add_saturating (state->normal_latency.samples, state->pager_latency.samples);
    combined_latency.total_ns = add_saturating (state->normal_latency.total_ns, state->pager_latency.total_ns);

    combined_latency.max_ns =
        state->normal_latency.max_ns > state->pager_latency.max_ns
            ? state->normal_latency.max_ns
            : state->pager_latency.max_ns;

    for (unsigned bucket_index = 0; bucket_index < USFS_STATUS_LATENCY_BUCKETS; ++bucket_index)
        combined_latency.buckets[bucket_index] = add_saturating (state->normal_latency.buckets[bucket_index], state->pager_latency.buckets[bucket_index]);

    return combined_latency;
}

static const char * kext_state_code_to_string (const uint32_t state)
{
    switch (state)
    {
        case USFS_KEXT_STATE_DOWN:
            return "DOWN";
        case USFS_KEXT_STATE_STARTING:
            return "STARTING";
        case USFS_KEXT_STATE_ACTIVE:
            return "ACTIVE";
        case USFS_KEXT_STATE_STOPPING:
            return "STOPPING";
        case USFS_KEXT_STATE_CLEANUP_REQUIRED:
            return "CLEANUP_REQUIRED";
        default:
            return "UNKNOWN";
    }
}

static const char * kext_fault_code_to_string (const uint32_t fault)
{
    switch (fault)
    {
        case USFS_FAULT_TIMEOUT:
            return "timeout";
        case USFS_FAULT_PROTOCOL:
            return "protocol";
        case USFS_FAULT_DAEMON_LOST:
            return "daemon-lost";
        case USFS_FAULT_FORCED_RECOVERY:
            return "forced-recovery";
        case USFS_FAULT_CLEANUP_REQUIRED:
            return "cleanup-required";
        default:
            return "unknown";
    }
}

static void print_latency (const struct usfs_latency_stats * latency)
{
    if (latency->samples == 0)
    {
        printf ("latency: no samples\n");
        return;
    }

    const uint32_t median_latency_ms = usfs_status_percentile_ms (latency, 50u);
    const uint32_t percentile_95_latency_ms = usfs_status_percentile_ms (latency, 95u);
    uint64_t maximum_latency_ms = latency->max_ns / 1000000u;

    if (latency->max_ns % 1000000u != 0)
        maximum_latency_ms += 1u;

    printf ("latency: p50<=%ums, p95<=%ums, max=%llums\n", median_latency_ms, percentile_95_latency_ms, (unsigned long long)maximum_latency_ms);
}

static void print_kext_state (const struct kext_state * state)
{
    const struct usfs_latency_stats latency = combine_latency_stats (state);

    uint64_t completed_request_count = state->successful_requests;
    completed_request_count = add_saturating (completed_request_count, state->daemon_error_requests);
    completed_request_count = add_saturating (completed_request_count, state->transport_failure_requests);
    completed_request_count = add_saturating (completed_request_count, state->timed_out_requests);
    completed_request_count = add_saturating (completed_request_count, state->interrupted_requests);

    const uint64_t failed_request_count = add_saturating (state->daemon_error_requests, state->transport_failure_requests);

    printf ("USFS health: %s\n", state->health == USFS_HEALTH_HEALTHY ? "HEALTHY" : "DEGRADED");
    printf ("kext: %s\n", kext_state_code_to_string (state->kext_state));
    printf ("mounts: %u active, %u stale\n", state->active_mounts, state->stale_mounts);
    printf ("daemons: %u active, %u unhealthy\n", state->active_daemons, state->unhealthy_daemons + state->dead_mounted_daemons);
    printf ("queue: %u outstanding, %u peak, limit %u\n", state->outstanding_requests, state->peak_outstanding_requests, state->max_outstanding_requests);
    printf ("cleanup reserve: 1 per connection, total per-connection bound %u\n", state->max_outstanding_requests + 1u);
    printf ("requests: %llu completed, %llu failed, %llu timed out\n", (unsigned long long)completed_request_count, (unsigned long long)failed_request_count, (unsigned long long)state->timed_out_requests);
    print_latency (&latency);
    printf ("settings: request=%ums, pager=%ums\n", state->request_timeout_ms, state->pager_timeout_ms);

    if (state->last_fault == USFS_FAULT_NONE)
    {
        printf ("last fault: none\n");
        return;
    }

    printf (
        "last fault: %s time=%llu channel=%d opcode=%u unique=%llu errno=%d\n",
        kext_fault_code_to_string (state->last_fault),
        (unsigned long long)state->last_fault_time_sec,
        state->last_fault_channel,
        (unsigned)state->last_fault_opcode,
        (unsigned long long)state->last_fault_unique,
        state->last_fault_errno
    );
}

static void print_usage (void)
{
    fprintf (
        stderr,
        "Failed to process command: invalid arguments\n\n"
        "Commands:\n"
        "  status            Show filesystem health and settings (default command).\n"
        "\n"
        "Tracing:\n"
        "  trace start [--profile PROFILE] [--output RAW_OUTPUT_PATH]\n"
        "    Start recording; requires root.\n\n"
        "  trace pause       Pause recording.\n"
        "  trace resume      Resume recording.\n"
        "  trace stop        Stop recording and create a text report.\n"
        "  trace status      Show the current trace session and output path.\n\n"
        "  trace report RAW_OUTPUT_PATH [--output REPORT_PATH]\n"
        "    Create a text report from an existing raw trace file.\n"
        "\n"
        "Defaults:\n"
        "  Profile:     requests (choices: core, requests, full)\n"
        "  Raw output:  /var/adm/ras/usfs-<UTC timestamp>-<PID>.trc\n"
        "  Report:      RAW_OUTPUT_PATH.txt\n"
        "\n"
        "Output paths must be absolute and refer to new files.\n"
    );
}

static int should_process_default_command (const int argc)
{
    const int user_argument_count = argc - 1;

    if (user_argument_count == 0)
        return true;

    return false;
}

static int should_process_status_command (const int argc, char ** argv)
{
    const int user_argument_count = argc - 1;

    if (user_argument_count != 1)
        return false;

    const char * command_name = argv[1];

    if (strcmp (command_name, "status") != 0)
        return false;

    return true;
}

static int should_process_trace_command (const int argc, char ** argv)
{
    const int user_argument_count = argc - 1;

    if (user_argument_count < 1)
        return false;

    const char * command_name = argv[1];

    if (strcmp (command_name, "trace") != 0)
        return false;

    return true;
}

static int process_status_command (void)
{
    struct kext_state state = { 0 };

    const int block_device_descriptor = open (status_device_file, O_RDONLY | O_NONBLOCK);

    if (block_device_descriptor < 0)
    {
        fprintf (stderr, "Failed to open device file %s: %s\n", status_device_file, strerror (errno));
        return USFSCTL_UNAVAILABLE;
    }

    if (ioctl (block_device_descriptor, USFS_IOC_GET_KEXT_STATE, &state) != 0)
    {
        fprintf (stderr, "Failed to read kernel extension state from device file %s: %s\n", status_device_file, strerror (errno));
        (void)close (block_device_descriptor);
        return USFSCTL_UNAVAILABLE;
    }

    if (close (block_device_descriptor) != 0)
    {
        fprintf (stderr, "Failed to close device file %s: %s\n", status_device_file, strerror (errno));
        return USFSCTL_UNAVAILABLE;
    }

    if (usfs_status_validate (&state) != 0)
    {
        fprintf (
            stderr,
            "Failed to validate kernel extension state from device file %s: incompatible or malformed response (ABI=%u, size=%u)\n",
            status_device_file,
            (unsigned)state.abi_version,
            (unsigned)state.size
        );
        return USFSCTL_UNAVAILABLE;
    }

    print_kext_state (&state);

    return state.health == USFS_HEALTH_HEALTHY
               ? USFS_SUCCESS
               : USFS_FAILURE;
}

static int process_trace_status_command (const int argc)
{
    if (argc != 3)
        return USFSCTL_WRONG_USAGE;

    struct usfsctl_trace_state state = { 0 };

    if (!read_and_verify_trace_session (&state))
    {
        if (access (USFSCTL_TRACE_STATE, F_OK) != 0)
        {
            printf ("Trace session inactive\n");
            return 0;
        }

        fprintf (stderr, "Failed to verify trace session ownership: state file %s does not identify a matching raw trace file and running trace daemon\n", USFSCTL_TRACE_STATE);
        return USFSCTL_UNAVAILABLE;
    }

    printf ("Verified trace session ownership: profile=%s raw_output_path=%s\n", state.trace_profile_name, state.trace_raw_output_path);
    return 0;
}

static int process_trace_pause_command (const int argc)
{
    if (argc != 3)
        return USFSCTL_WRONG_USAGE;

    struct usfsctl_trace_state state = { 0 };

    if (!read_and_verify_trace_session (&state))
    {
        fprintf (stderr, "Failed to verify trace session ownership: state file %s does not identify a matching raw trace file and running trace daemon\n", USFSCTL_TRACE_STATE);
        return USFSCTL_UNAVAILABLE;
    }

    const char * pause_command_args[] = { "/usr/bin/trcoff", NULL };

    return run_command (pause_command_args, NULL, "pause trace session") == 0 ? 0 : USFSCTL_UNAVAILABLE;
}

static int process_trace_resume_command (const int argc)
{
    if (argc != 3)
        return USFSCTL_WRONG_USAGE;

    struct usfsctl_trace_state state = { 0 };

    if (!read_and_verify_trace_session (&state))
    {
        fprintf (stderr, "Failed to verify trace session ownership: state file %s does not identify a matching raw trace file and running trace daemon\n", USFSCTL_TRACE_STATE);
        return USFSCTL_UNAVAILABLE;
    }

    const char * resume_command_args[] = { "/usr/bin/trcon", NULL };

    return run_command (resume_command_args, NULL, "resume trace session") == 0 ? 0 : USFSCTL_UNAVAILABLE;
}

static int process_trace_stop_command (const int argc)
{
    if (argc != 3)
        return USFSCTL_WRONG_USAGE;

    struct usfsctl_trace_state state = { 0 };

    if (!read_and_verify_trace_session (&state))
    {
        fprintf (stderr, "Failed to verify trace session ownership: state file %s does not identify a matching raw trace file and running trace daemon\n", USFSCTL_TRACE_STATE);
        return USFSCTL_UNAVAILABLE;
    }

    const char * stop_command_args[] = { "/usr/bin/trcstop", "-s", NULL };
    const int rc = run_command (stop_command_args, NULL, "stop trace session");

    if (rc != 0)
        return USFSCTL_UNAVAILABLE;

    remove_trace_state_file ();
    return write_trace_report (state.trace_raw_output_path, NULL);
}

static int process_trace_report_without_output (const int argc, char ** argv)
{
    if (argc != 4)
        return USFSCTL_WRONG_USAGE;

    const char * raw_trace_output_path = argv[3];
    const char * formatted_trace_report_path = NULL;

    return write_trace_report (raw_trace_output_path, formatted_trace_report_path);
}

static int process_trace_report_with_output (const int argc, char ** argv)
{
    if (argc != 6)
        return USFSCTL_WRONG_USAGE;

    const char * output_option_name = argv[4];

    if (strcmp (output_option_name, "--output") != 0)
        return USFSCTL_WRONG_USAGE;

    const char * raw_trace_output_path = argv[3];
    const char * formatted_trace_report_path = argv[5];

    return write_trace_report (raw_trace_output_path, formatted_trace_report_path);
}

static int process_trace_report_command (const int argc, char ** argv)
{
    if (argc == 4)
        return process_trace_report_without_output (argc, argv);

    if (argc == 6)
        return process_trace_report_with_output (argc, argv);

    return USFSCTL_WRONG_USAGE;
}

static int prepare_raw_trace_output_path (struct usfsctl_trace_start_options * options, char * generated_raw_trace_output_path, const size_t path_capacity)
{
    if (options->raw_trace_output_path == NULL)
    {
        if (make_default_trace_path (generated_raw_trace_output_path, path_capacity) != 0)
        {
            fprintf (stderr, "Failed to generate raw trace output path: could not format timestamp or fit path in %lu bytes\n", (unsigned long)path_capacity);
            return USFSCTL_UNAVAILABLE;
        }

        options->raw_trace_output_path = generated_raw_trace_output_path;
    }

    const char * raw_trace_output_path = options->raw_trace_output_path;

    if (!is_valid_trace_path (raw_trace_output_path))
    {
        fprintf (stderr, "Failed to start trace session: invalid raw output path %s\n", raw_trace_output_path);
        return USFSCTL_UNAVAILABLE;
    }

    if (access (raw_trace_output_path, F_OK) == 0)
    {
        fprintf (stderr, "Failed to start trace session: raw output file %s already exists\n", raw_trace_output_path);
        return USFSCTL_UNAVAILABLE;
    }

    return 0;
}

static int start_trace_daemon (const char * trace_profile_name, const char * trace_hook_ids, const char * raw_trace_output_path, time_t * earliest_start_time)
{
    char trace_session_description[256];

    // todo: check API hardcode
    (void)snprintf (trace_session_description, sizeof (trace_session_description), "USFS trace ABI 1 profile=%s package=%s revision=%s", trace_profile_name, USFS_PACKAGE_VERSION, USFS_BUILD_REVISION);

    *earliest_start_time = time (NULL);

    if (*earliest_start_time == (time_t)-1)
    {
        fprintf (stderr, "Failed to read trace session start time: %s\n", strerror (errno));
        return -1;
    }

    const char * trace_start_command_args[] = {
        "/usr/bin/trace",          // AIX trace daemon executable.
        "-a",                      // Run the daemon asynchronously in the background.
        "-d",                      // Defer event collection until trcon enables recording.
        "-j",                      // Select the trace hook IDs supplied by the next argument.
        trace_hook_ids,            // Comma-separated USFS hook IDs for the selected profile.
        "-J",                      // Include the event group supplied by the next argument.
        "tidhk",                   // Context hooks needed to identify processes and threads in reports.
        "-p",                      // Record the current processor ID with each trace hook.
        "-T",                      // Set the in-memory trace buffer size in bytes.
        "1048576",                 // Use a 1 MiB trace buffer.
        "-L",                      // Set the trace log size limit in bytes.
        "16777216",                // Use a 16 MiB log, which wraps when full by default.
        "-m",                      // Set the message stored in the trace log header.
        trace_session_description, // Record the trace ABI, profile, package version, and revision.
        "-o",                      // Select the raw trace output file.
        raw_trace_output_path,     // Absolute path to the raw trace output file.
        NULL                       // Terminate the argument array passed to execv.
    };

    return run_command (trace_start_command_args, NULL, "start trace session");
}

static int record_trace_session_ownership (const char * trace_profile_name, const char * raw_trace_output_path, const time_t earliest_start_time)
{
    struct stat raw_trace_file_metadata;

    if (stat (raw_trace_output_path, &raw_trace_file_metadata) != 0)
    {
        fprintf (stderr, "Failed to inspect raw trace output file %s: %s\n", raw_trace_output_path, strerror (errno));
        return -1; // todo: review return codes of functions in general, maybe we should have unified contract
    }

    psinfo_t trace_daemon_process_info;

    if (find_trace_daemon (earliest_start_time, &trace_daemon_process_info) != 0)
        return -1;

    return write_trace_state (trace_profile_name, raw_trace_output_path, &raw_trace_file_metadata, &trace_daemon_process_info);
}

static int parse_and_validate_trace_start_request (const int argc, char ** argv, struct usfsctl_trace_start_options * options, const char ** trace_hook_ids)
{
    if (geteuid () != 0)
    {
        fprintf (stderr, "Failed to start trace session: root privileges are required (effective UID=%lu)\n", (unsigned long)geteuid ());
        return USFSCTL_UNAVAILABLE;
    }

    if (parse_trace_start_options (argc - 3, argv + 3, options) != 0)
        return USFSCTL_WRONG_USAGE;

    const char * trace_profile_name = options->trace_profile_name;
    *trace_hook_ids = get_trace_hooks (trace_profile_name);

    if (*trace_hook_ids == NULL)
    {
        fprintf (stderr, "Failed to select trace profile %s: expected core, requests, or full\n", trace_profile_name);
        return USFSCTL_WRONG_USAGE;
    }

    return 0;
}

// todo: check USFSCTL_UNAVAILABLE usages and make sure it's correct to use this return code in all those places

static int prepare_trace_start_paths (struct usfsctl_trace_start_options * options, char * generated_raw_trace_output_path, const size_t path_capacity)
{
    if (prepare_raw_trace_output_path (options, generated_raw_trace_output_path, path_capacity) != 0)
        return USFSCTL_UNAVAILABLE;

    if (access (USFSCTL_TRACE_STATE, F_OK) == 0)
    {
        fprintf (stderr, "Failed to start trace session: ownership state file %s already exists\n", USFSCTL_TRACE_STATE);
        return USFSCTL_UNAVAILABLE;
    }

    return 0;
}

static int start_trace_session (const struct usfsctl_trace_start_options * options, const char * trace_hook_ids)
{
    const char * trace_profile_name = options->trace_profile_name;
    const char * raw_trace_output_path = options->raw_trace_output_path;
    time_t earliest_trace_daemon_start_time;

    if (start_trace_daemon (trace_profile_name, trace_hook_ids, raw_trace_output_path, &earliest_trace_daemon_start_time) != 0)
        return USFSCTL_UNAVAILABLE;

    const char * trace_discard_command_args[] = { "/usr/bin/trcstop", "-d", NULL };

    if (record_trace_session_ownership (trace_profile_name, raw_trace_output_path, earliest_trace_daemon_start_time) != 0)
    {
        (void)run_command (trace_discard_command_args, NULL, "discard trace session");
        return USFSCTL_UNAVAILABLE;
    }

    const char * trace_enable_command_args[] = { "/usr/bin/trcon", NULL };

    if (run_command (trace_enable_command_args, NULL, "enable trace recording") != 0)
    {
        (void)run_command (trace_discard_command_args, NULL, "discard trace session");
        remove_trace_state_file ();
        return USFSCTL_UNAVAILABLE;
    }

    printf ("Started trace session: profile=%s raw_output_path=%s\n", trace_profile_name, raw_trace_output_path);
    return 0;
}

static int process_trace_start_command (const int argc, char ** argv)
{
    struct usfsctl_trace_start_options trace_start_options;
    const char * trace_hook_ids = NULL;
    const int rc = parse_and_validate_trace_start_request (argc, argv, &trace_start_options, &trace_hook_ids);

    if (rc != 0)
        return rc;

    char generated_raw_trace_output_path[PATH_MAX] = { 0 };

    if (prepare_trace_start_paths (&trace_start_options, generated_raw_trace_output_path, sizeof (generated_raw_trace_output_path)) != 0)
        return USFSCTL_UNAVAILABLE;

    return start_trace_session (&trace_start_options, trace_hook_ids);
}

static int process_trace_command (const int argc, char ** argv)
{
    if (argc < 3)
        return USFSCTL_WRONG_USAGE;

    const char * trace_action_name = argv[2];

    if (strcmp (trace_action_name, "start") == 0)
        return process_trace_start_command (argc, argv);

    if (strcmp (trace_action_name, "status") == 0)
        return process_trace_status_command (argc);

    if (strcmp (trace_action_name, "pause") == 0)
        return process_trace_pause_command (argc);

    if (strcmp (trace_action_name, "resume") == 0)
        return process_trace_resume_command (argc);

    if (strcmp (trace_action_name, "stop") == 0)
        return process_trace_stop_command (argc);

    if (strcmp (trace_action_name, "report") == 0)
        return process_trace_report_command (argc, argv);

    return USFSCTL_WRONG_USAGE;
}

static int process_default_command (void)
{
    return process_status_command ();
}

static int process_command (const int argc, char ** argv)
{
    if (should_process_default_command (argc))
        return process_default_command ();

    if (should_process_status_command (argc, argv))
        return process_status_command ();

    if (should_process_trace_command (argc, argv))
        return process_trace_command (argc, argv);

    return USFSCTL_WRONG_USAGE;
}

int main (const int argc, char ** argv)
{
    (void)umask (077); // todo: review this

    const int rc = process_command (argc, argv);

    if (rc == USFSCTL_WRONG_USAGE)
        print_usage ();

    return rc;
}

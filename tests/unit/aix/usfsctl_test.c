// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "tap.h"
#include "usfs_common.h"
#include "usfs_status.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/procfs.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static psinfo_t process_info;
static struct stat file_metadata;
static struct kext_state kernel_state;
static const char * state_text;
static const char * existing_path;
static int open_result;
static int close_result;
static int ioctl_result;
static int stat_result;
static int short_read;
static int proxy_state;
static int implementation_state;
static int removal_result;
static unsigned removal_calls;
static unsigned close_calls;
static unsigned unlink_calls;
static uid_t effective_uid;
static pid_t fork_result;
static int child_status;
static int wait_error;
static int directory_available;
static unsigned process_entries;
static unsigned directory_cursor;

static int fixture_open (const char * path, const int flags, ...)
{
    (void)path;
    (void)flags;
    errno = EACCES;
    return open_result;
}

static int fixture_close (const int descriptor)
{
    (void)descriptor;
    close_calls++;
    errno = EIO;
    return close_result;
}

static ssize_t fixture_read (const int descriptor, void * buffer, const size_t size)
{
    (void)descriptor;
    if (size != sizeof (process_info))
        abort ();

    memcpy (buffer, &process_info, size);
    return short_read ? 0 : (ssize_t)size;
}

static int fixture_ioctl (const int descriptor, const int command, ...)
{
    (void)descriptor;
    (void)command;
    va_list arguments;

    va_start (arguments, command);
    struct kext_state * reply = va_arg (arguments, struct kext_state *);
    *reply = kernel_state;
    va_end (arguments);
    errno = EIO;
    return ioctl_result;
}

static FILE * fixture_fopen (const char * path, const char * mode)
{
    (void)path;
    (void)mode;

    if (state_text == NULL)
        return NULL;

    FILE * stream = tmpfile ();
    if (stream == NULL)
        abort ();

    fputs (state_text, stream);
    rewind (stream);
    return stream;
}

static FILE * fixture_fdopen (const int descriptor, const char * mode)
{
    (void)descriptor;
    (void)mode;
    errno = ENOMEM;
    return NULL;
}

static int fixture_stat (const char * path, struct stat * metadata)
{
    (void)path;
    *metadata = file_metadata;
    errno = ENOENT;
    return stat_result;
}

static int fixture_access (const char * path, const int mode)
{
    (void)mode;
    return existing_path != NULL && strcmp (path, existing_path) == 0 ? 0 : -1;
}

static int fixture_unlink (const char * path)
{
    (void)path;
    unlink_calls++;
    errno = EACCES;
    return -1;
}

static uid_t fixture_geteuid (void)
{
    return effective_uid;
}

static pid_t fixture_fork (void)
{
    // No test may launch a trace command or modify the installed service.
    if (fork_result == 0)
        abort ();

    errno = EAGAIN;
    return fork_result;
}

static pid_t fixture_waitpid (const pid_t process_id, int * status, const int options)
{
    (void)options;
    *status = child_status;
    errno = ECHILD;
    return wait_error ? -1 : process_id;
}

static int fixture_isatty (const int descriptor)
{
    (void)descriptor;
    return 0;
}

static DIR * fixture_opendir (const char * path)
{
    (void)path;
    directory_cursor = 0;
    errno = EACCES;
    return directory_available ? (DIR *)&directory_cursor : NULL;
}

static struct dirent * fixture_readdir (DIR * directory)
{
    (void)directory;
    static struct dirent entry;

    if (directory_cursor++ >= process_entries)
        return NULL;

    strcpy (entry.d_name, "123");
    return &entry;
}

static int fixture_closedir (DIR * directory)
{
    (void)directory;
    return 0;
}

int usfs_proxy_query_user (mid_t * module, struct usfs_proxy_status * status)
{
    *module = proxy_state == USFS_QUERY_PRESENT ? 123 : 0;
    memset (status, 0, sizeof (*status));
    errno = EIO;
    return proxy_state;
}

int usfs_kext_is_loaded (void)
{
    errno = EACCES;
    return implementation_state;
}

mid_t usfs_kext_get_module_id (void)
{
    return implementation_state == USFS_QUERY_PRESENT ? 456 : 0;
}

int usfs_proxy_unsafe_unload (void)
{
    removal_calls++;
    return removal_result;
}

#define open fixture_open
#define close fixture_close
#define read fixture_read
#define ioctl fixture_ioctl
#define fopen fixture_fopen
#define fdopen fixture_fdopen
#define stat(...) fixture_stat (__VA_ARGS__)
#define access fixture_access
#define unlink fixture_unlink
#define geteuid fixture_geteuid
#define fork fixture_fork
#define waitpid fixture_waitpid
#define isatty fixture_isatty
#define opendir fixture_opendir
#define readdir fixture_readdir
#define closedir fixture_closedir
#define main usfsctl_main
#include "../../../src/cli/usfsctl.c"
#undef main

static void reset_fixture (void)
{
    memset (&process_info, 0, sizeof (process_info));
    strcpy (process_info.pr_fname, "trace");
    strcpy (process_info.pr_psargs, "/usr/bin/trace -a -d");
    process_info.pr_pid = 123;
    process_info.pr_start.tv_sec = 10;
    process_info.pr_start.tv_nsec = 20;
    file_metadata.st_dev = 30;
    file_metadata.st_ino = 40;
    state_text = "requests /trace.raw 30 40 123 10 20\n";
    existing_path = NULL;
    open_result = 17;
    close_result = 0;
    ioctl_result = 0;
    stat_result = 0;
    short_read = 0;
    proxy_state = USFS_QUERY_PRESENT;
    implementation_state = USFS_QUERY_PRESENT;
    removal_result = USFS_SUCCESS;
    removal_calls = 0;
    close_calls = 0;
    unlink_calls = 0;
    effective_uid = 0;
    fork_result = -1;
    child_status = 0;
    wait_error = 0;
    directory_available = true;
    process_entries = 1;
    memset (&kernel_state, 0, sizeof (kernel_state));
    kernel_state.magic = USFS_STATUS_MAGIC;
    kernel_state.abi_version = USFS_STATUS_ABI_VERSION;
    kernel_state.size = sizeof (kernel_state);
    kernel_state.health = USFS_HEALTH_HEALTHY;
    kernel_state.kext_state = USFS_KEXT_STATE_ACTIVE;
}

static void verify_trace_identity (struct tap_state * tap)
{
    struct usfsctl_trace_state state = { 0 };
    reset_fixture ();
    tap_ok (tap, read_and_verify_trace_session (&state), "trace ownership requires matching file and daemon identity");

    const char * invalid_states[] = { NULL, "truncated", "requests /trace.raw 30 40 0 10 20", "requests relative 30 40 123 10 20" };
    for (unsigned state_index = 0; state_index < sizeof (invalid_states) / sizeof (invalid_states[0]); state_index++)
    {
        state_text = invalid_states[state_index];
        tap_ok (tap, !read_and_verify_trace_session (&state), "missing or malformed ownership state is rejected");
    }

    for (unsigned mismatch = 0; mismatch < 10; mismatch++)
    {
        reset_fixture ();
        switch (mismatch)
        {
            case 0: stat_result = -1; break;
            case 1: open_result = -1; break;
            case 2: short_read = 1; break;
            case 3: process_info.pr_euid = 1; break;
            case 4: strcpy (process_info.pr_fname, "other"); break;
            case 5: strcpy (process_info.pr_psargs, "trace"); break;
            case 6: file_metadata.st_dev++; break;
            case 7: file_metadata.st_ino++; break;
            case 8: process_info.pr_start.tv_sec++; break;
            case 9: process_info.pr_start.tv_nsec++; break;
        }
        tap_ok (tap, !read_and_verify_trace_session (&state), "ownership rejects unavailable, replaced, or reused file/process identity");
    }

    reset_fixture ();
    const char * invalid_pids[] = { "", "abc", "0", "-1", "2147483648" };
    for (unsigned pid_index = 0; pid_index < sizeof (invalid_pids) / sizeof (invalid_pids[0]); pid_index++)
        tap_ok (tap, !read_matching_trace_daemon (invalid_pids[pid_index], 0, &process_info), "process discovery rejects malformed or out-of-range IDs");

    tap_ok (tap, !read_matching_trace_daemon ("123", 11, &process_info), "process discovery rejects an older daemon");
    psinfo_t discovered = { 0 };
    directory_available = false;
    tap_ok (tap, find_trace_daemon (0, &discovered) == -1, "process directory failure is reported");
    directory_available = true;
    process_entries = 0;
    tap_ok (tap, find_trace_daemon (0, &discovered) == -1, "missing trace daemon is rejected");
    process_entries = 2;
    tap_ok (tap, find_trace_daemon (0, &discovered) == -1, "ambiguous trace daemon identity is rejected");
    process_entries = 1;
    tap_ok (tap, find_trace_daemon (0, &discovered) == 0 && discovered.pr_pid == 123, "unique trace daemon is identified");
}

static void verify_paths_and_options (struct tap_state * tap)
{
    reset_fixture ();
    const char * invalid_paths[] = { NULL, "relative", "/space path", "/tab\tpath", "/line\npath", "/return\rpath" };
    for (unsigned path_index = 0; path_index < sizeof (invalid_paths) / sizeof (invalid_paths[0]); path_index++)
        tap_ok (tap, !is_valid_trace_path (invalid_paths[path_index]), "trace paths reject missing, relative, and whitespace-containing names");

    char long_path[PATH_MAX + 1];
    memset (long_path, 'x', sizeof (long_path));
    long_path[0] = '/';
    long_path[PATH_MAX] = '\0';
    tap_ok (tap, !is_valid_trace_path (long_path), "trace path rejects the capacity boundary");
    long_path[PATH_MAX - 1] = '\0';
    tap_ok (tap, write_trace_report (long_path, NULL) == USFSCTL_WRONG_USAGE, "generated report suffix must fit the path capacity");
    tap_ok (tap, write_trace_report (NULL, NULL) == USFSCTL_WRONG_USAGE, "missing raw trace path is a usage error");
    tap_ok (tap, write_trace_report ("/raw", "relative") == USFSCTL_UNAVAILABLE, "relative report destination is rejected");
    existing_path = "/report";
    tap_ok (tap, write_trace_report ("/raw", "/report") == USFSCTL_UNAVAILABLE, "report generation never overwrites an existing file");
    existing_path = NULL;
    tap_ok (tap, write_trace_report ("/raw", NULL) == USFSCTL_UNAVAILABLE, "report command launch failure is preserved");

    struct usfsctl_trace_start_options options = { 0 };
    char * unknown[] = { "--unknown", "value" };
    char * missing[] = { "--output" };
    tap_ok (tap, parse_trace_start_options (2, unknown, &options) == USFSCTL_WRONG_USAGE, "unknown trace option is rejected");
    tap_ok (tap, parse_trace_start_options (1, missing, &options) == USFSCTL_WRONG_USAGE, "trace option requires its value");
    tap_ok (tap, strcmp (get_trace_hooks ("core"), "F5F1") == 0 && strcmp (get_trace_hooks ("full"), "F5F1,F5F2,F5F3") == 0 && get_trace_hooks ("invalid") == NULL,
            "trace profiles map to their documented hooks");

    char generated_path[PATH_MAX] = { 0 };
    tap_ok (tap, make_default_trace_path (generated_path, sizeof (generated_path)) == 0 && is_valid_trace_path (generated_path), "default trace path is absolute and valid");
    tap_ok (tap, make_default_trace_path (generated_path, 1) == -1, "default trace path detects truncation");
    options.raw_trace_output_path = NULL;
    tap_ok (tap, prepare_raw_trace_output_path (&options, generated_path, 1) == USFSCTL_UNAVAILABLE, "trace preparation propagates default-path truncation");
    options.raw_trace_output_path = NULL;
    tap_ok (tap, prepare_raw_trace_output_path (&options, generated_path, sizeof (generated_path)) == 0, "trace preparation generates a default path");
    options.raw_trace_output_path = "relative";
    tap_ok (tap, prepare_raw_trace_output_path (&options, generated_path, sizeof (generated_path)) == USFSCTL_UNAVAILABLE, "trace preparation rejects a relative raw path");
    options.raw_trace_output_path = "/raw";
    existing_path = USFSCTL_TRACE_STATE;
    tap_ok (tap, prepare_trace_start_paths (&options, generated_path, sizeof (generated_path)) == USFSCTL_UNAVAILABLE, "existing ownership state prevents starting another session");
}

static void verify_status_failures (struct tap_state * tap)
{
    for (unsigned failure = 0; failure < 6; failure++)
    {
        reset_fixture ();
        switch (failure)
        {
            case 0: proxy_state = USFS_QUERY_ERROR; break;
            case 1: implementation_state = USFS_QUERY_ERROR; break;
            case 2: open_result = -1; break;
            case 3: ioctl_result = -1; break;
            case 4: close_result = -1; break;
            case 5: kernel_state.abi_version++; break;
        }
        const unsigned expected_closes = failure >= 3 ? 1 : 0;
        tap_ok (tap, process_status_command () == USFSCTL_UNAVAILABLE && close_calls == expected_closes,
                "status failures close acquired descriptors and report unavailable");
    }

    reset_fixture ();
    tap_ok (tap, process_status_command () == USFS_SUCCESS && close_calls == 1, "valid healthy status succeeds and closes the device");
    kernel_state.health = USFS_HEALTH_DEGRADED;
    tap_ok (tap, process_status_command () == USFS_FAILURE, "degraded health is a failure result");
    tap_ok (tap, add_saturating (UINT64_MAX, 1) == UINT64_MAX && add_saturating (10, 20) == 30, "status aggregation saturates instead of wrapping");
    kernel_state.normal_latency.samples = UINT64_MAX;
    kernel_state.pager_latency.samples = 1;
    kernel_state.normal_latency.max_ns = 20;
    kernel_state.pager_latency.max_ns = 10;
    const struct usfs_latency_stats combined = combine_latency_stats (&kernel_state);
    tap_ok (tap, combined.samples == UINT64_MAX && combined.max_ns == 20, "combined latency saturates sample counts and selects the larger maximum");
}

static void verify_command_failures (struct tap_state * tap)
{
    reset_fixture ();
    char * invalid[] = { "usfsctl", "unknown", NULL };
    tap_ok (tap, usfsctl_main (2, invalid) == USFSCTL_WRONG_USAGE, "unknown command returns usage status");
    char * proxy[] = { "usfsctl", "proxy", "unload", "--unsafe", "--confirm-unsafe-unload", NULL };
    tap_ok (tap, process_command (3, proxy) == USFSCTL_WRONG_USAGE && removal_calls == 0, "proxy removal requires the unsafe flag");
    effective_uid = 1;
    tap_ok (tap, process_command (5, proxy) == USFS_FAILURE && removal_calls == 0, "unprivileged caller cannot remove the proxy");
    effective_uid = 0;
    tap_ok (tap, process_command (4, proxy) == USFS_FAILURE && removal_calls == 0, "noninteractive caller must explicitly confirm removal");
    removal_result = USFS_FAILURE;
    tap_ok (tap, process_command (5, proxy) == USFS_FAILURE && removal_calls == 1, "confirmed maintenance preserves backend failure");

    char * trace[] = { "usfsctl", "trace", "start", "--profile", "invalid", NULL };
    tap_ok (tap, process_command (2, trace) == USFSCTL_WRONG_USAGE, "trace command requires an action");
    effective_uid = 1;
    tap_ok (tap, process_command (5, trace) == USFSCTL_UNAVAILABLE, "trace start requires root");
    effective_uid = 0;
    tap_ok (tap, process_command (5, trace) == USFSCTL_WRONG_USAGE, "trace start rejects unknown profile");
    tap_ok (tap, process_command (4, trace) == USFSCTL_WRONG_USAGE, "trace start rejects an incomplete option pair");
    state_text = NULL;
    tap_ok (tap, process_trace_status_command (3) == 0, "missing ownership file means inactive trace");
    existing_path = USFSCTL_TRACE_STATE;
    tap_ok (tap, process_trace_status_command (3) == USFSCTL_UNAVAILABLE, "unreadable existing ownership state is unavailable");
    tap_ok (tap, process_trace_resume_command (3) == USFSCTL_UNAVAILABLE, "resume refuses unverifiable ownership");

    reset_fixture ();
    tap_ok (tap, process_trace_pause_command (3) == USFSCTL_UNAVAILABLE, "pause propagates command launch failure");
    tap_ok (tap, process_trace_resume_command (3) == USFSCTL_UNAVAILABLE, "resume propagates command launch failure");
    tap_ok (tap, process_trace_stop_command (3) == USFSCTL_UNAVAILABLE && unlink_calls == 0, "failed stop retains ownership state");
    open_result = -1;
    tap_ok (tap, write_trace_state ("requests", "/raw", &file_metadata, &process_info) == -1, "state creation failure is reported");
    open_result = 17;
    const unsigned previous_closes = close_calls;
    tap_ok (tap, write_trace_state ("requests", "/raw", &file_metadata, &process_info) == -1 && close_calls == previous_closes + 1 && unlink_calls == 1,
            "stream creation failure closes descriptor and removes ownership file");
    wait_error = 1;
    tap_ok (tap, wait_for_command (123, "test command") == -1, "wait failure is reported");
    wait_error = 0;
    child_status = SIGTERM;
    tap_ok (tap, wait_for_command (123, "test command") == -1, "signalled command is reported as failed");
    child_status = 7 << 8;
    tap_ok (tap, wait_for_command (123, "test command") == 7, "command exit status is retained");
}

int main (void)
{
    struct tap_state tap;

    tap_plan (&tap, 76);
    verify_trace_identity (&tap);
    verify_paths_and_options (&tap);
    verify_status_failures (&tap);
    verify_command_failures (&tap);

    return tap_finish (&tap);
}

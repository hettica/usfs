/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Bounded deterministic filesystem workload for authorized reliability tests.
 * It deliberately limits object count and I/O size; the coordinator supplies
 * a private mount, stop/pause files, and a recorded seed for every process.
 */

#define _ALL_SOURCE     1
#define _DEFAULT_SOURCE 1

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
    #define PATH_MAX 4096
#endif

enum worker_mode
{
    MODE_MUTABLE,
    MODE_READONLY
};

struct options
{
    const char * root;     // Mounted filesystem root path.
    const char * status;   // Published worker status file path.
    const char * stop;     // Path whose presence requests shutdown.
    const char * pause;    // Path whose presence pauses work.
    enum worker_mode mode; // Selected filesystem workload.
    uint64_t seed;         // Recorded deterministic random seed.
    unsigned duty;         // Requested work duty percentage.
    unsigned batch;        // Iterations per workload batch.
    unsigned max_files;    // Maximum private file slots.
    unsigned fail_after;   // Iteration threshold for intentional failure.
};

static volatile sig_atomic_t stopping;
static uint64_t random_state;

static void on_signal (int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static uint64_t next_random (void)
{
    uint64_t value = random_state;

    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    random_state = value;
    return value;
}

static int parse_unsigned (const char * text, uint64_t minimum, uint64_t maximum, uint64_t * result)
{
    unsigned long long value;
    char * parse_end = NULL;

    if (text == NULL || text[0] == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    value = strtoull (text, &parse_end, 10);
    if (errno != 0 || parse_end == text || *parse_end != '\0' || value < minimum ||
        value > maximum)
        return -1;
    *result = (uint64_t)value;
    return 0;
}

static int take_value (int argc, char ** argv, int * item_index, const char ** value)
{
    if (*item_index + 1 >= argc)
        return -1;
    *value = argv[++(*item_index)];
    return 0;
}

static int parse_worker_mode (const char * argument, enum worker_mode * mode)
{
    if (strcmp (argument, "mutable") == 0)
        *mode = MODE_MUTABLE;
    else if (strcmp (argument, "readonly") == 0)
        *mode = MODE_READONLY;
    else
        return -1;

    return 0;
}

static int parse_unsigned_option (const int argc, char ** argv, int * argument_index, const uint64_t minimum, const uint64_t maximum, unsigned * result)
{
    const char * argument = NULL;
    if (take_value (argc, argv, argument_index, &argument) != 0)
        return -1;

    uint64_t numeric_value;
    if (parse_unsigned (argument, minimum, maximum, &numeric_value) != 0)
        return -1;

    *result = (unsigned)numeric_value;
    return 0;
}

static int parse_worker_option (const int argc, char ** argv, int * argument_index, struct options * options)
{
    const char * option_name = argv[*argument_index];
    if (strcmp (option_name, "--root") == 0)
        return take_value (argc, argv, argument_index, &options->root);
    if (strcmp (option_name, "--status") == 0)
        return take_value (argc, argv, argument_index, &options->status);
    if (strcmp (option_name, "--stop") == 0)
        return take_value (argc, argv, argument_index, &options->stop);
    if (strcmp (option_name, "--pause") == 0)
        return take_value (argc, argv, argument_index, &options->pause);
    if (strcmp (option_name, "--mode") == 0)
    {
        const char * argument = NULL;
        if (take_value (argc, argv, argument_index, &argument) != 0)
            return -1;

        return parse_worker_mode (argument, &options->mode);
    }
    if (strcmp (option_name, "--seed") == 0)
    {
        const char * argument = NULL;
        if (take_value (argc, argv, argument_index, &argument) != 0)
            return -1;

        return parse_unsigned (argument, 1, UINT64_MAX, &options->seed);
    }
    if (strcmp (option_name, "--duty") == 0)
        return parse_unsigned_option (argc, argv, argument_index, 1, 80, &options->duty);
    if (strcmp (option_name, "--batch") == 0)
        return parse_unsigned_option (argc, argv, argument_index, 1, 128, &options->batch);
    if (strcmp (option_name, "--max-files") == 0)
        return parse_unsigned_option (argc, argv, argument_index, 4, 256, &options->max_files);
    if (strcmp (option_name, "--fail-after") == 0)
        return parse_unsigned_option (argc, argv, argument_index, 1, 1000000000u, &options->fail_after);

    return -1;
}

static int parse_options (const int argc, char ** argv, struct options * options)
{
    memset (options, 0, sizeof (*options));
    options->duty = 60;
    options->batch = 16;
    options->max_files = 64;
    for (int argument_index = 1; argument_index < argc; ++argument_index)
    {
        if (parse_worker_option (argc, argv, &argument_index, options) != 0)
            return -1;
    }

    if (options->root == NULL)
        return -1;
    if (options->status == NULL)
        return -1;
    if (options->stop == NULL)
        return -1;
    if (options->pause == NULL)
        return -1;

    return options->seed != 0 ? 0 : -1;
}

static int path_join (char * output, size_t output_size, const char * left, const char * right)
{
    int length = snprintf (output, output_size, "%s/%s", left, right);

    return length < 0 || (size_t)length >= output_size ? -1 : 0;
}

static int prepare_private (const char * path)
{
    struct stat metadata;

    if (mkdir (path, 0700) == 0)
        return 0;
    if (errno != EEXIST)
        return -1;
    if (stat (path, &metadata) != 0)
        return -1;
    if (!S_ISDIR (metadata.st_mode))
    {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int write_status (const struct options * options, const char * state, uint64_t sequence, const char * operation, int error)
{
    char temporary_status_path[PATH_MAX];
    FILE * stream;
    int length;

    length = snprintf (temporary_status_path, sizeof (temporary_status_path), "%s.tmp.%ld", options->status, (long)getpid ());
    if (length < 0 || (size_t)length >= sizeof (temporary_status_path))
        return -1;
    stream = fopen (temporary_status_path, "w");
    if (stream == NULL)
        return -1;
    fprintf (stream, "state=%s\npid=%ld\nseed=%llu\nsequence=%llu\noperation=%s\n"
                     "errno=%d\ntimestamp=%ld\n",
             state,
             (long)getpid (),
             (unsigned long long)options->seed,
             (unsigned long long)sequence,
             operation,
             error,
             (long)time (NULL));
    if (fclose (stream) != 0 || rename (temporary_status_path, options->status) != 0)
    {
        unlink (temporary_status_path);
        return -1;
    }
    return 0;
}

static void fill_pattern (unsigned char * buffer, size_t size, uint64_t token)
{
    size_t item_index;

    for (item_index = 0; item_index < size; ++item_index)
        buffer[item_index] = (unsigned char)((token + item_index * 17u) & 0xffu);
}

static int verify_pattern (int file_descriptor, const unsigned char * expected, size_t size)
{
    unsigned char actual[4096];
    size_t done = 0;

    if (lseek (file_descriptor, 0, SEEK_SET) < 0)
        return -1;
    while (done < size)
    {
        size_t wanted = size - done;
        ssize_t count;

        if (wanted > sizeof (actual))
            wanted = sizeof (actual);
        count = read (file_descriptor, actual, wanted);
        if (count <= 0 || memcmp (actual, expected + done, (size_t)count) != 0)
        {
            errno = EIO;
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}

static int allowed_race_error (int error)
{
    return error == ENOENT || error == EEXIST || error == ENOTEMPTY ||
           error == EISDIR || error == ENOTDIR;
}

struct mutable_paths
{
    char file[PATH_MAX];        // Original private file path.
    char renamed[PATH_MAX];     // Renamed private file path.
    char hard[PATH_MAX];        // Hard link path.
    char symbolic[PATH_MAX];    // Symbolic link path.
    char subdir[PATH_MAX];      // Private subdirectory path.
    char child[PATH_MAX];       // Child file in the private subdirectory.
    char shared[PATH_MAX];      // Directory shared by competing workers.
    char shared_file[PATH_MAX]; // File shared by competing workers.
};

static int prepare_mutable_paths (const struct options * options, const char * private_directory, uint64_t sequence, struct mutable_paths * paths)
{
    if (snprintf (paths->file, sizeof (paths->file), "%s/f%03u", private_directory, (unsigned)(sequence % options->max_files)) >=
            (int)sizeof (paths->file) ||
        snprintf (paths->renamed, sizeof (paths->renamed), "%s/r%03u", private_directory, (unsigned)(sequence % options->max_files)) >=
            (int)sizeof (paths->renamed) ||
        path_join (paths->hard, sizeof (paths->hard), private_directory, "hard") != 0 ||
        path_join (paths->symbolic, sizeof (paths->symbolic), private_directory, "symbolic") != 0 ||
        path_join (paths->subdir, sizeof (paths->subdir), private_directory, "subdir") != 0 ||
        path_join (paths->child, sizeof (paths->child), paths->subdir, "child") != 0 ||
        path_join (paths->shared, sizeof (paths->shared), options->root, ".usfs-soak-shared") != 0 ||
        path_join (paths->shared_file, sizeof (paths->shared_file), paths->shared, "race") != 0)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int exercise_file_contents (const char * file, uint64_t token, size_t size, const char ** operation)
{
    unsigned char expected[8192];
    struct stat metadata;
    void * mapping;
    int file_descriptor;

    *operation = "private-write-verify";
    unlink (file);
    file_descriptor = open (file, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (file_descriptor < 0)
        return -1;
    fill_pattern (expected, size, token);
    if (write (file_descriptor, expected, size) != (ssize_t)size || fsync (file_descriptor) != 0 ||
        verify_pattern (file_descriptor, expected, size) != 0)
    {
        close (file_descriptor);
        return -1;
    }

    *operation = "mmap-msync";
    mapping = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, 0);
    if (mapping != MAP_FAILED)
    {
        ((unsigned char *)mapping)[size / 2] ^= 0x5au;
        expected[size / 2] ^= 0x5au;
        if (msync (mapping, size, MS_SYNC) != 0 || munmap (mapping, size) != 0 ||
            verify_pattern (file_descriptor, expected, size) != 0)
        {
            close (file_descriptor);
            return -1;
        }
    }
    else if (errno != ENOSYS && errno != ENOTSUP)
    {
        close (file_descriptor);
        return -1;
    }

    *operation = "append-truncate-metadata";
    if (lseek (file_descriptor, 0, SEEK_END) < 0 || write (file_descriptor, "A", 1) != 1 ||
        ftruncate (file_descriptor, (off_t)(size / 2)) != 0 || fchmod (file_descriptor, 0640) != 0 ||
        fchown (file_descriptor, getuid (), getgid ()) != 0 || fstat (file_descriptor, &metadata) != 0)
    {
        close (file_descriptor);
        return -1;
    }
    close (file_descriptor);
    return 0;
}

static int exercise_links (const struct mutable_paths * paths, const char ** operation)
{
    struct stat metadata;
    unsigned char byte;
    int file_descriptor;

    *operation = "links-rename";
    unlink (paths->hard);
    unlink (paths->symbolic);
    unlink (paths->renamed);
    if (link (paths->file, paths->hard) != 0 ||
        symlink (paths->file, paths->symbolic) != 0 ||
        rename (paths->file, paths->renamed) != 0 ||
        lstat (paths->symbolic, &metadata) != 0)
    {
        return -1;
    }
    unlink (paths->hard);
    unlink (paths->symbolic);

    *operation = "open-unlink";
    file_descriptor = open (paths->renamed, O_RDONLY);
    if (file_descriptor < 0 || unlink (paths->renamed) != 0 || read (file_descriptor, &byte, 1) != 1)
    {
        if (file_descriptor >= 0)
            close (file_descriptor);
        return -1;
    }
    close (file_descriptor);
    return 0;
}

static int exercise_namespace (const char * private_directory, const struct mutable_paths * paths, const char ** operation)
{
    struct statvfs filesystem_status;
    DIR * directory;
    int file_descriptor;

    *operation = "namespace-traversal";
    mkdir (paths->subdir, 0700);
    file_descriptor = open (paths->child, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (file_descriptor < 0 || close (file_descriptor) != 0)
        return -1;
    directory = opendir (private_directory);
    if (directory == NULL)
        return -1;
    /* Each DIR stream belongs exclusively to this single-threaded worker. */
    /* cppcheck-suppress readdirCalled */
    while (readdir (directory) != NULL)
        ;
    if (closedir (directory) != 0 || statvfs (private_directory, &filesystem_status) != 0 ||
        access (paths->child, R_OK | W_OK) != 0)
        return -1;
    errno = 0;
    if (rmdir (paths->subdir) == 0 || errno != ENOTEMPTY)
        return -1;
    unlink (paths->child);
    rmdir (paths->subdir);
    return 0;
}

static int exercise_shared_race (const struct mutable_paths * paths, uint64_t token, const char ** operation)
{
    int file_descriptor;

    *operation = "shared-mkdir";
    if (mkdir (paths->shared, 0770) != 0 && errno != EEXIST)
        return -1;
    *operation = "shared-open";
    file_descriptor = open (paths->shared_file, O_CREAT | O_RDWR, 0660);
    if (file_descriptor < 0 && !allowed_race_error (errno))
        return -1;
    if (file_descriptor >= 0)
    {
        *operation = "shared-write";
        if (write (file_descriptor, &token, sizeof (token)) < 0 && !allowed_race_error (errno))
        {
            close (file_descriptor);
            return -1;
        }
        close (file_descriptor);
    }
    *operation = "shared-unlink";
    if (unlink (paths->shared_file) != 0 && !allowed_race_error (errno))
        return -1;
    return 0;
}

static int mutable_iteration (const struct options * options, const char * private_directory, uint64_t sequence, const char ** operation)
{
    struct mutable_paths paths;
    uint64_t token = next_random ();
    size_t size = 1024u + (size_t)(token % 7168u);

    if (prepare_mutable_paths (options, private_directory, sequence, &paths) != 0 ||
        exercise_file_contents (paths.file, token, size, operation) != 0 ||
        exercise_links (&paths, operation) != 0 ||
        exercise_namespace (private_directory, &paths, operation) != 0 ||
        exercise_shared_race (&paths, token, operation) != 0)
        return -1;
    return 0;
}

static int readonly_iteration (const struct options * options, const char ** operation)
{
    char path[PATH_MAX];
    char buffer[4096];
    struct statvfs filesystem_status;
    struct stat metadata;
    DIR * directory;
    ssize_t count;
    int file_descriptor;

    *operation = "mirror-read";
    if (path_join (path, sizeof (path), options->root, "file000") != 0)
        return -1;
    file_descriptor = open (path, O_RDONLY);
    if (file_descriptor < 0)
        return -1;
    count = read (file_descriptor, buffer, sizeof (buffer));
    if (count <= 0 || fstat (file_descriptor, &metadata) != 0 || close (file_descriptor) != 0)
        return -1;
    directory = opendir (options->root);
    if (directory == NULL)
        return -1;
    /* Each DIR stream belongs exclusively to this single-threaded worker. */
    /* cppcheck-suppress readdirCalled */
    while (readdir (directory) != NULL)
        ;
    if (closedir (directory) != 0 || statvfs (options->root, &filesystem_status) != 0)
        return -1;
    errno = 0;
    file_descriptor = open (path, O_WRONLY);
    if (file_descriptor >= 0)
    {
        close (file_descriptor);
        errno = EROFS;
        return -1;
    }
    return errno == EROFS || errno == EACCES ? 0 : -1;
}

static void cleanup_private (const char * private_directory)
{
    DIR * directory = opendir (private_directory);
    struct dirent * entry;
    char path[PATH_MAX];

    if (directory == NULL)
        return;
    /* Cleanup runs synchronously after this worker has stopped operations. */
    /* cppcheck-suppress readdirCalled */
    while ((entry = readdir (directory)) != NULL)
    {
        if (strcmp (entry->d_name, ".") == 0 || strcmp (entry->d_name, "..") == 0)
            continue;
        if (path_join (path, sizeof (path), private_directory, entry->d_name) == 0)
        {
            unlink (path);
            rmdir (path);
        }
    }
    closedir (directory);
    rmdir (private_directory);
}

static int report_worker_failure (const struct options * options, const char * private_directory, const uint64_t sequence, const char * operation, const int saved_errno)
{
    write_status (options, "failed", sequence, operation, saved_errno);
    fprintf (stderr, "worker failure pid=%ld seed=%llu sequence=%llu "
                     "operation=%s errno=%d (%s)\n",
             (long)getpid (),
             (unsigned long long)options->seed,
             (unsigned long long)sequence,
             operation,
             saved_errno,
             strerror (saved_errno));
    cleanup_private (private_directory);
    return 1;
}

static int run_worker_iteration (const struct options * options, const char * private_directory, const uint64_t sequence, const char ** operation)
{
    int result;
    if (options->fail_after != 0 && sequence >= options->fail_after)
    {
        errno = EIO;
        *operation = "intentional-failure";
        result = -1;
    }
    else if (options->mode == MODE_MUTABLE)
    {
        result = mutable_iteration (options, private_directory, sequence, operation);
    }
    else
    {
        result = readonly_iteration (options, operation);
    }
    return result;
}

static void wait_worker_duty_cycle (const struct options * options, const time_t work_start, const time_t work_end)
{
    if (options->duty < 100)
    {
        unsigned long work_seconds = (unsigned long)(work_end - work_start);
        unsigned long sleep_milliseconds;

        if (work_seconds == 0)
            work_seconds = 1;
        sleep_milliseconds = work_seconds * 1000u *
                             (100u - options->duty) / options->duty;
        if (sleep_milliseconds > 5000u)
            sleep_milliseconds = 5000u;
        sleep ((unsigned int)(sleep_milliseconds / 1000u));
        usleep ((unsigned int)((sleep_milliseconds % 1000u) * 1000u));
    }
}

static int run_worker_campaign (const struct options * options, const char * private_directory)
{
    const char * operation = "startup";
    uint64_t sequence = 0;
    time_t status_deadline = 0;

    write_status (options, "running", sequence, operation, 0);
    while (!stopping && access (options->stop, F_OK) != 0)
    {
        unsigned item_index;
        int was_paused = 0;
        time_t work_start;
        time_t work_end;

        while (!stopping && access (options->stop, F_OK) != 0 &&
               access (options->pause, F_OK) == 0)
        {
            was_paused = 1;
            write_status (options, "paused", sequence, operation, 0);
            sleep (1);
        }
        if (stopping || access (options->stop, F_OK) == 0)
            break;
        if (was_paused && options->mode == MODE_MUTABLE)
        {
            operation = "private-reinitialize";
            if (prepare_private (private_directory) != 0)
            {
                return report_worker_failure (options, private_directory, sequence, operation, errno);
            }
        }
        work_start = time (NULL);
        for (item_index = 0; item_index < options->batch && !stopping; ++item_index)
        {
            int result;

            ++sequence;
            result = run_worker_iteration (options, private_directory, sequence, &operation);
            if (result != 0)
            {
                return report_worker_failure (options, private_directory, sequence, operation, errno);
            }
        }
        work_end = time (NULL);
        if (work_end >= status_deadline)
        {
            write_status (options, "running", sequence, operation, 0);
            status_deadline = work_end + 60;
        }
        if ((sequence % 256u) == 0)
            sync ();
        wait_worker_duty_cycle (options, work_start, work_end);
    }
    write_status (options, "completed", sequence, operation, 0);
    cleanup_private (private_directory);
    return 0;
}

int main (const int argc, char ** argv)
{
    struct options options;
    char private_directory[PATH_MAX];

    if (parse_options (argc, argv, &options) != 0)
    {
        fprintf (stderr, "usage: %s --root PATH --mode mutable|readonly "
                         "--seed N --status FILE --stop FILE --pause FILE "
                         "[--duty 1..80] [--batch 1..128] [--max-files 4..256] "
                         "[--fail-after N]\n",
                 argv[0]);
        return 2;
    }
    random_state = options.seed;
    signal (SIGTERM, on_signal);
    signal (SIGINT, on_signal);
    if (snprintf (private_directory, sizeof (private_directory), "%s/.usfs-soak-%ld-%llu", options.root, (long)getpid (), (unsigned long long)options.seed) >= (int)sizeof (private_directory))
    {
        fprintf (stderr, "worker private path is too long\n");
        return 2;
    }
    if (options.mode == MODE_MUTABLE && prepare_private (private_directory) != 0)
    {
        perror ("worker mkdir");
        return 1;
    }
    return run_worker_campaign (&options, private_directory);
}

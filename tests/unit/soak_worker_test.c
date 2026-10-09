/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#define main usfs_soak_worker_program
#include "../../src/tools/usfs_soak_worker.c"
#undef main

#include "tap.h"

int main(void)
{
    struct tap_state tap;
    struct options options;
    char root_template[] = "/tmp/usfs-soak-unit-root.XXXXXX";
    char status_template[] = "/tmp/usfs-soak-unit-status.XXXXXX";
    char stop_path[PATH_MAX];
    char pause_path[PATH_MAX];
    char private_path[PATH_MAX];
    char *root;
    struct stat private_stat;
    int status_fd;
    uint64_t first;
    uint64_t second;
    FILE *stream;
    char status_text[1024];
    char *valid[] = { "worker", "--root", "/tmp", "--mode", "mutable",
        "--seed", "42", "--status", "/tmp/status", "--stop", "/tmp/stop",
        "--pause", "/tmp/pause", "--duty", "60", "--batch", "8",
        "--max-files", "32" };
    char *invalid[] = { "worker", "--root", "/tmp", "--mode", "mutable",
        "--seed", "0", "--status", "/tmp/status", "--stop", "/tmp/stop",
        "--pause", "/tmp/pause" };
    char *failure_argv[17];

    tap_plan(&tap, 9);
    tap_ok(&tap, parse_options(19, valid, &options) == 0 &&
                 options.seed == 42 && options.duty == 60 &&
                 options.batch == 8 && options.max_files == 32,
           "worker accepts bounded arguments");
    tap_ok(&tap, parse_options(13, invalid, &options) != 0,
           "worker rejects a zero seed");

    random_state = 0x12345678u;
    first = next_random();
    second = next_random();
    random_state = 0x12345678u;
    tap_ok(&tap, next_random() == first && next_random() == second,
           "recorded seed reproduces the operation stream");

    tap_ok(&tap, allowed_race_error(ENOENT) && allowed_race_error(EEXIST) &&
                 allowed_race_error(ENOTEMPTY) && !allowed_race_error(EIO),
           "shared-race allowlist excludes integrity errors");

    options.status = status_template;
    options.seed = 99;
    status_fd = mkstemp(status_template);
    if (status_fd >= 0)
        close(status_fd);
    tap_ok(&tap, status_fd >= 0 &&
                 write_status(&options, "running", 7, "unit-operation", 0) == 0,
           "progress status is replaced atomically");
    stream = fopen(status_template, "r");
    memset(status_text, 0, sizeof(status_text));
    if (stream != NULL) {
        (void)fread(status_text, 1, sizeof(status_text) - 1, stream);
        status_text[sizeof(status_text) - 1] = '\0';
        fclose(stream);
    }
    tap_ok(&tap, strstr(status_text, "seed=99") != NULL &&
                 strstr(status_text, "sequence=7") != NULL &&
                 strstr(status_text, "operation=unit-operation") != NULL,
           "progress status records reproducibility context");

    root = mkdtemp(root_template);
    snprintf(private_path, sizeof(private_path), "%s/private", root_template);
    tap_ok(&tap, root != NULL && prepare_private(private_path) == 0 &&
                 stat(private_path, &private_stat) == 0 &&
                 S_ISDIR(private_stat.st_mode),
           "worker prepares its private namespace");
    if (root != NULL)
        rmdir(private_path);
    tap_ok(&tap, root != NULL && prepare_private(private_path) == 0 &&
                 stat(private_path, &private_stat) == 0 &&
                 S_ISDIR(private_stat.st_mode),
           "worker recreates private namespace after volatile restart");
    snprintf(stop_path, sizeof(stop_path), "%s.stop", status_template);
    snprintf(pause_path, sizeof(pause_path), "%s.pause", status_template);
    failure_argv[0] = "worker";
    failure_argv[1] = "--root"; failure_argv[2] = root_template;
    failure_argv[3] = "--mode"; failure_argv[4] = "mutable";
    failure_argv[5] = "--seed"; failure_argv[6] = "123";
    failure_argv[7] = "--status"; failure_argv[8] = status_template;
    failure_argv[9] = "--stop"; failure_argv[10] = stop_path;
    failure_argv[11] = "--pause"; failure_argv[12] = pause_path;
    failure_argv[13] = "--fail-after"; failure_argv[14] = "1";
    failure_argv[15] = "--batch"; failure_argv[16] = "1";
    stopping = 0;
    tap_ok(&tap, root != NULL && usfs_soak_worker_program(17, failure_argv) == 1,
           "intentional failure follows the normal failure-capture path");

    unlink(status_template);
    unlink(stop_path);
    unlink(pause_path);
    if (root != NULL) {
        rmdir(private_path);
        rmdir(root);
    }
    return tap_finish(&tap);
}

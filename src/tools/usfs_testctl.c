/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "usfs_test.h"

#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_uint (const char * text, const unsigned long maximum, unsigned * value)
{
    char * parse_end = NULL;

    errno = 0;
    const unsigned long parsed_value = strtoul (text, &parse_end, 0);
    if (errno != 0)
        return -1;

    if (parse_end == text)
        return -1;

    if (*parse_end != '\0')
        return -1;

    if (parsed_value > maximum)
        return -1;

    *value = (unsigned)parsed_value;
    return 0;
}

static int run_info (const int device_descriptor, char ** argv)
{
    struct usfs_test_info info;

    (void)argv;
    memset (&info, 0, sizeof (info));
    if (ioctl (device_descriptor, USFS_TEST_IOC_INFO, &info) != 0)
    {
        perror ("USFS_TEST_IOC_INFO");
        return 1;
    }
    if (info.abi_version != USFS_TEST_ABI_VERSION ||
        info.fault_max != USFS_TEST_FAULT_MAX ||
        info.schedule_max != USFS_TEST_SCHEDULE_MAX)
    {
        fprintf (stderr, "test ABI mismatch or malformed INFO\n");
        return 1;
    }
    printf ("ABI=%u faults=%u schedules=%u flags=%#x\n", info.abi_version, info.fault_max, info.schedule_max, info.flags);
    return 0;
}

static int run_status (const int device_descriptor, char ** argv)
{
    struct usfs_test_status status;

    (void)argv;
    memset (&status, 0, sizeof (status));
    if (ioctl (device_descriptor, USFS_TEST_IOC_STATUS, &status) != 0)
    {
        perror ("USFS_TEST_IOC_STATUS");
        return 1;
    }
    printf ("fault=%u errno=%d occurrence=%u observed=%u fired=%u armed=%u\n", status.fault, status.error, status.occurrence, status.observed, status.fired, status.armed);
    return 0;
}

static int run_reset (const int device_descriptor, char ** argv)
{
    (void)argv;
    if (ioctl (device_descriptor, USFS_TEST_IOC_RESET, 0) != 0)
    {
        perror ("USFS_TEST_IOC_RESET");
        return 1;
    }
    return 0;
}

static int run_arm (const int device_descriptor, char ** argv)
{
    struct usfs_test_arm arm;
    unsigned fault;
    unsigned occurrence;
    unsigned error;

    if (parse_uint (argv[0], USFS_TEST_FAULT_MAX, &fault) != 0 ||
        parse_uint (argv[1], 0xfffffffful, &occurrence) != 0 ||
        parse_uint (argv[2], 127, &error) != 0 ||
        fault == 0 || occurrence == 0 || error == 0)
    {
        fprintf (stderr, "invalid arm arguments\n");
        return 2;
    }
    memset (&arm, 0, sizeof (arm));
    arm.abi_version = USFS_TEST_ABI_VERSION;
    arm.fault = fault;
    arm.occurrence = occurrence;
    arm.error = (int32_t)error;
    if (ioctl (device_descriptor, USFS_TEST_IOC_ARM, &arm) != 0)
    {
        perror ("USFS_TEST_IOC_ARM");
        return 1;
    }
    return 0;
}

static int run_schedule (const int device_descriptor, char ** argv)
{
    struct usfs_test_schedule_arm arm;
    unsigned schedule;

    if (parse_uint (argv[0], USFS_TEST_SCHEDULE_MAX, &schedule) != 0 ||
        schedule == USFS_TEST_SCHEDULE_NONE)
    {
        fprintf (stderr, "invalid schedule argument\n");
        return 2;
    }
    memset (&arm, 0, sizeof (arm));
    arm.abi_version = USFS_TEST_ABI_VERSION;
    arm.schedule = schedule;
    if (ioctl (device_descriptor, USFS_TEST_IOC_SCHEDULE_ARM, &arm) != 0)
    {
        perror ("USFS_TEST_IOC_SCHEDULE_ARM");
        return 1;
    }

    if (schedule == USFS_TEST_SCHEDULE_CHANNEL_DEALLOC_TERM)
    {
        printf ("channel deallocation schedule armed\n");
        if (fflush (stdout) != 0)
        {
            fprintf (stderr, "Failed to publish channel deallocation schedule readiness: %s\n", strerror (errno));
            return 1;
        }
    }

    return 0;
}

static int run_schedule_status (const int device_descriptor, char ** argv)
{
    struct usfs_test_schedule_status status;

    (void)argv;
    memset (&status, 0, sizeof (status));
    if (ioctl (device_descriptor, USFS_TEST_IOC_SCHEDULE_STATUS, &status) != 0)
    {
        perror ("USFS_TEST_IOC_SCHEDULE_STATUS");
        return 1;
    }
    if (status.abi_version != USFS_TEST_ABI_VERSION ||
        status.reserved != 0)
    {
        fprintf (stderr, "test schedule ABI mismatch or malformed status\n");
        return 1;
    }
    printf ("schedule=%u reached=%#x channel_pre=%u channel_rejected=%u timed_out=%u active=%u lookup_post=%u lookup_resolved=%u\n", status.schedule, status.reached, status.channel_pre_count, status.channel_rejected_count, status.timed_out, status.active, status.lookup_post_count, status.lookup_resolved_count);
    return 0;
}

static int run_node_status (const int device_descriptor, char ** argv)
{
    struct usfs_test_node_status status;

    (void)argv;
    memset (&status, 0, sizeof (status));
    if (ioctl (device_descriptor, USFS_TEST_IOC_NODE_STATUS, &status) != 0)
    {
        perror ("USFS_TEST_IOC_NODE_STATUS");
        return 1;
    }
    if (status.abi_version != USFS_TEST_ABI_VERSION ||
        status.reserved != 0)
    {
        fprintf (stderr, "test node ABI mismatch or malformed status\n");
        return 1;
    }
    printf ("created=%llu reclaimed=%llu live=%llu peak=%llu lookup_reused=%llu parent_reused=%llu create_reused=%llu parent_rebuilt=%llu holds=%llu releases=%llu root_created=%llu root_reclaimed=%llu duplicate_live=%llu accounting_errors=%llu\n", (unsigned long long)status.created, (unsigned long long)status.reclaimed, (unsigned long long)status.live, (unsigned long long)status.peak_live, (unsigned long long)status.lookup_reused, (unsigned long long)status.parent_reused, (unsigned long long)status.create_reused, (unsigned long long)status.parent_rebuilt, (unsigned long long)status.hold_calls, (unsigned long long)status.release_calls, (unsigned long long)status.root_created, (unsigned long long)status.root_reclaimed, (unsigned long long)status.duplicate_live, (unsigned long long)status.accounting_errors);
    return 0;
}

static int run_selftest_case (const int device_descriptor, const unsigned case_id)
{
    struct usfs_test_selftest selftest;

    memset (&selftest, 0, sizeof (selftest));
    selftest.abi_version = USFS_TEST_ABI_VERSION;
    selftest.case_id = case_id;
    if (ioctl (device_descriptor, USFS_TEST_IOC_SELFTEST, &selftest) != 0)
    {
        perror ("USFS_TEST_IOC_SELFTEST");
        return 1;
    }
    if (selftest.abi_version != USFS_TEST_ABI_VERSION ||
        selftest.case_id != case_id || !selftest.passed)
    {
        fprintf (stderr, "selftest %u failed: passed=%u observed=%d\n", case_id, selftest.passed, selftest.observed);
        return 1;
    }
    printf ("selftest %u passed (observed=%d)\n", case_id, selftest.observed);
    return 0;
}

static int run_selftest (const int device_descriptor, char ** argv)
{
    unsigned first_case;
    unsigned last_case;
    unsigned case_id;

    if (strcmp (argv[0], "all") == 0)
    {
        first_case = 1;
        last_case = USFS_TEST_SELFTEST_MAX;
    }
    else if (parse_uint (argv[0], USFS_TEST_SELFTEST_MAX, &first_case) != 0 || first_case == USFS_TEST_SELFTEST_NONE)
    {
        fprintf (stderr, "invalid selftest case\n");
        return 2;
    }
    else
    {
        last_case = first_case;
    }

    for (case_id = first_case; case_id <= last_case; case_id++)
    {
        if (run_selftest_case (device_descriptor, case_id) != 0)
            return 1;
    }
    return 0;
}

typedef int (*command_handler) (const int device_descriptor, char ** argv);

struct command
{
    const char * name;   // Command name accepted at the command line.
    int argument_count;  // Required arguments after the command name.
    command_handler run; // Handler for the validated command arguments.
};

static int dispatch_command (const int device_descriptor, const int argc, char ** argv)
{
    static const struct command commands[] = {
        { "info", 0, run_info },
        { "status", 0, run_status },
        { "node-status", 0, run_node_status },
        { "reset", 0, run_reset },
        { "arm", 3, run_arm },
        { "schedule", 1, run_schedule },
        { "schedule-status", 0, run_schedule_status },
        { "selftest", 1, run_selftest }
    };
    size_t command_index;

    for (command_index = 0; command_index < sizeof (commands) / sizeof (commands[0]); ++command_index)
    {
        if (strcmp (argv[0], commands[command_index].name) == 0 &&
            argc == commands[command_index].argument_count + 1)
            return commands[command_index].run (device_descriptor, argv + 1);
    }
    fprintf (stderr, "invalid command or arguments\n");
    return 2;
}

int main (const int argc, char ** argv)
{
    if (argc < 2)
    {
        fprintf (stderr, "usage: usfs_testctl info|status|node-status|reset|arm FAULT OCCURRENCE ERRNO|schedule ID|schedule-status|selftest all|ID\n");
        return 2;
    }
    const int device_descriptor = open ("/dev/usfs0", O_RDWR);
    if (device_descriptor < 0)
    {
        perror ("open /dev/usfs0");
        return 1;
    }
    const int rc = dispatch_command (device_descriptor, argc - 1, argv + 1);
    close (device_descriptor);
    return rc;
}

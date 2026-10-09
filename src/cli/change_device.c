/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * chusfs executable - USFS device Change method.
 *
 * Invoked by the ODM configuration framework when an administrator changes
 * settings for a configured USFS device, typically through `chdev`. It can
 * also be run directly to inspect the current configuration or adjust
 * supported filesystem settings while the device remains online.
 *
 * The method holds the ODM configuration lock across the complete operation.
 */

#include "usfs_common.h"
#include "usfs_file.h"

static const char * device_name_option = "-l";
static const char * attribute_assignment_option = "-a";

struct change_command_options
{
    const char * logical_device_name;         // Logical name of the device to inspect or change.
    const char * attribute_assignments[32];   // Attribute name=value arguments, in command-line order.
    unsigned number_of_attribute_assignments; // Number of populated entries in attribute_assignments.
};

static const char * get_option_value (const int argc, char ** argv, const int argument_index)
{
    if (argument_index + 1 >= argc)
        return NULL;

    return argv[argument_index + 1];
}

static int parse_uint32 (const char * value_text, const uint32_t minimum_value, const uint32_t maximum_value, uint32_t * parsed_value)
{
    if (value_text == NULL)
        return USFS_FAILURE;

    if (value_text[0] == '\0')
        return USFS_FAILURE;

    if (value_text[0] == '-')
        return USFS_FAILURE;

    char * end_of_value = NULL;
    errno = 0;
    const unsigned long numeric_value = strtoul (value_text, &end_of_value, 10);

    if (errno != 0)
        return USFS_FAILURE;

    if (end_of_value == value_text)
        return USFS_FAILURE;

    if (*end_of_value != '\0')
        return USFS_FAILURE;

    if (numeric_value < minimum_value)
        return USFS_FAILURE;

    if (numeric_value > maximum_value)
        return USFS_FAILURE;

    *parsed_value = (uint32_t)numeric_value;
    return USFS_SUCCESS;
}

static int is_attribute_named (const char * attribute_assignment, const size_t attribute_name_length, const char * expected_attribute_name)
{
    if (strncmp (attribute_assignment, expected_attribute_name, attribute_name_length) != 0)
        return false;

    if (expected_attribute_name[attribute_name_length] != '\0')
        return false;

    return true;
}

static int apply_request_timeout (struct kext_runtime_config * runtime_configuration, const char * attribute_value_text)
{
    uint32_t request_timeout_ms;

    if (parse_uint32 (attribute_value_text, USFS_MIN_TIMEOUT_MS, USFS_MAX_TIMEOUT_MS, &request_timeout_ms) != USFS_SUCCESS)
        return USFS_FAILURE;

    runtime_configuration->request_timeout_ms = request_timeout_ms;
    runtime_configuration->set_mask |= USFS_RUNTIME_SET_REQUEST_TIMEOUT;

    return USFS_SUCCESS;
}

static int apply_pager_timeout (struct kext_runtime_config * runtime_configuration, const char * attribute_value_text)
{
    uint32_t pager_timeout_ms;

    if (parse_uint32 (attribute_value_text, USFS_MIN_TIMEOUT_MS, USFS_MAX_TIMEOUT_MS, &pager_timeout_ms) != USFS_SUCCESS)
        return USFS_FAILURE;

    runtime_configuration->pager_timeout_ms = pager_timeout_ms;
    runtime_configuration->set_mask |= USFS_RUNTIME_SET_PAGER_TIMEOUT;

    return USFS_SUCCESS;
}

static int apply_max_outstanding_requests (struct kext_runtime_config * runtime_configuration, const char * attribute_value_text)
{
    uint32_t max_outstanding_requests;

    if (parse_uint32 (attribute_value_text, USFS_MIN_OUTSTANDING, USFS_MAX_OUTSTANDING, &max_outstanding_requests) != USFS_SUCCESS)
        return USFS_FAILURE;

    runtime_configuration->max_outstanding_requests = max_outstanding_requests;
    runtime_configuration->set_mask |= USFS_RUNTIME_SET_MAX_OUTSTANDING;

    return USFS_SUCCESS;
}

static int apply_attribute_assignment (struct kext_runtime_config * runtime_configuration, const char * attribute_assignment)
{
    const char * assignment_separator = strchr (attribute_assignment, '=');

    if (assignment_separator == NULL)
        return USFS_FAILURE;

    if (assignment_separator == attribute_assignment)
        return USFS_FAILURE;

    const size_t attribute_name_length = (size_t)(assignment_separator - attribute_assignment);
    const char * attribute_value_text = assignment_separator + 1;

    if (is_attribute_named (attribute_assignment, attribute_name_length, "request_timeout_ms"))
        return apply_request_timeout (runtime_configuration, attribute_value_text);

    if (is_attribute_named (attribute_assignment, attribute_name_length, "pager_timeout_ms"))
        return apply_pager_timeout (runtime_configuration, attribute_value_text);

    if (is_attribute_named (attribute_assignment, attribute_name_length, "max_outstanding_requests"))
        return apply_max_outstanding_requests (runtime_configuration, attribute_value_text);

    return USFS_FAILURE;
}

static int open_control_device (const char * logical_device_name)
{
    if (logical_device_name == NULL)
    {
        fprintf (stderr, "Failed to open device file: logical device name is missing\n");
        return INVALID_FILE_DESCRIPTOR;
    }

    if (logical_device_name[0] == '\0')
    {
        fprintf (stderr, "Failed to open device file: logical device name is empty\n");
        return INVALID_FILE_DESCRIPTOR;
    }

    if (strchr (logical_device_name, '/') != NULL)
    {
        fprintf (stderr, "Failed to open device file: logical device name %s contains a slash\n", logical_device_name);
        return INVALID_FILE_DESCRIPTOR;
    }

    char control_device_path[USFS_DEVICE_PATH_MAX];
    const int control_device_path_length = snprintf (control_device_path, sizeof (control_device_path), "/dev/%s", logical_device_name);

    if (control_device_path_length < 0)
    {
        fprintf (stderr, "Failed to format device file path for %s: %s\n", logical_device_name, strerror (errno));
        return INVALID_FILE_DESCRIPTOR;
    }

    if ((size_t)control_device_path_length >= sizeof (control_device_path))
    {
        fprintf (stderr, "Failed to format device file path for %s: path exceeds %lu bytes\n", logical_device_name, sizeof (control_device_path) - 1);
        return INVALID_FILE_DESCRIPTOR;
    }

    const int control_device_fd = open (control_device_path, O_RDONLY | O_NONBLOCK);

    if (control_device_fd < 0)
        fprintf (stderr, "Failed to open device file %s: %s\n", control_device_path, strerror (errno));

    return control_device_fd;
}

static void print_runtime_configuration (const struct kext_runtime_config * runtime_configuration)
{
    printf ("request_timeout_ms=%u\n", runtime_configuration->request_timeout_ms);
    printf ("pager_timeout_ms=%u\n", runtime_configuration->pager_timeout_ms);
    printf ("max_outstanding_requests=%u\n", runtime_configuration->max_outstanding_requests);
    printf ("max_total_outstanding_requests=%u\n", runtime_configuration->max_outstanding_requests + 1u);
    printf ("active_connections=%u\n", runtime_configuration->active_connections);
    printf ("unhealthy_connections=%u\n", runtime_configuration->unhealthy_connections);
    printf ("outstanding_requests=%u\n", runtime_configuration->outstanding_requests);
}

static int parse_device_name_option (const int argc, char ** argv, const int argument_index, struct change_command_options * command_options)
{
    if (command_options->logical_device_name != NULL)
    {
        fprintf (stderr, "Failed to parse device name: option %s was specified more than once\n", device_name_option);
        return USFS_FAILURE;
    }

    command_options->logical_device_name = get_option_value (argc, argv, argument_index);

    if (command_options->logical_device_name == NULL)
    {
        fprintf (stderr, "Failed to parse device name: option %s requires a value\n", device_name_option);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

static int parse_attribute_assignment_option (const int argc, char ** argv, const int argument_index, struct change_command_options * command_options)
{
    const char * attribute_assignment = get_option_value (argc, argv, argument_index);

    if (command_options->logical_device_name == NULL)
    {
        fprintf (stderr, "Failed to parse attribute assignment: option %s must follow %s and its device name\n", attribute_assignment_option, device_name_option);
        return USFS_FAILURE;
    }

    if (attribute_assignment == NULL)
    {
        fprintf (stderr, "Failed to parse attribute assignment: option %s requires name=value\n", attribute_assignment_option);
        return USFS_FAILURE;
    }

    const size_t maximum_number_of_assignments = sizeof (command_options->attribute_assignments) / sizeof (command_options->attribute_assignments[0]);

    if (command_options->number_of_attribute_assignments == maximum_number_of_assignments)
    {
        fprintf (stderr, "Failed to add attribute assignment %s: maximum is %lu assignments\n", attribute_assignment, (unsigned long)maximum_number_of_assignments);
        return USFS_FAILURE;
    }

    command_options->attribute_assignments[command_options->number_of_attribute_assignments++] = attribute_assignment;
    return USFS_SUCCESS;
}

static int parse_command_option (const int argc, char ** argv, const int argument_index, struct change_command_options * command_options)
{
    const char * option_name = argv[argument_index];

    if (strcmp (option_name, device_name_option) == 0)
        return parse_device_name_option (argc, argv, argument_index, command_options);

    if (strcmp (option_name, attribute_assignment_option) == 0)
        return parse_attribute_assignment_option (argc, argv, argument_index, command_options);

    fprintf (stderr, "Failed to parse command option %s: expected %s or %s\n", option_name, device_name_option, attribute_assignment_option);
    return USFS_FAILURE;
}

static int parse_command_options (const int argc, char ** argv, struct change_command_options * command_options)
{
    for (int argument_index = 1; argument_index < argc; argument_index += 2)
    {
        if (parse_command_option (argc, argv, argument_index, command_options) != USFS_SUCCESS)
            return USFS_FAILURE;
    }

    if (command_options->logical_device_name == NULL)
    {
        fprintf (stderr, "Failed to parse command options: missing %s and its device name\n", device_name_option);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

static int read_runtime_configuration (const int control_device_fd, struct kext_runtime_config * runtime_configuration)
{
    memset (runtime_configuration, 0, sizeof (*runtime_configuration));

    if (ioctl (control_device_fd, USFS_IOC_GET_RUNTIME_CONFIG, runtime_configuration) != 0)
    {
        fprintf (stderr, "Failed to read runtime configuration from device descriptor %d: %s\n", control_device_fd, strerror (errno));
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

static int apply_attribute_assignments (struct kext_runtime_config * runtime_configuration, const struct change_command_options * command_options)
{
    for (unsigned assignment_index = 0; assignment_index < command_options->number_of_attribute_assignments; ++assignment_index)
    {
        const char * attribute_assignment = command_options->attribute_assignments[assignment_index];

        if (apply_attribute_assignment (runtime_configuration, attribute_assignment) != USFS_SUCCESS)
        {
            fprintf (stderr, "Failed to apply attribute assignment %s: unsupported attribute, invalid integer, or value outside the permitted range\n", attribute_assignment);
            return USFS_FAILURE;
        }
    }

    return USFS_SUCCESS;
}

static void prepare_runtime_configuration (struct kext_runtime_config * runtime_configuration)
{
    runtime_configuration->magic = USFS_RUNTIME_CONFIG_MAGIC;
    runtime_configuration->abi_version = USFS_RUNTIME_CONFIG_ABI_VERSION;
    runtime_configuration->size = sizeof (*runtime_configuration);

    if (runtime_configuration->set_mask != 0)
    {
        runtime_configuration->active_connections = 0;
        runtime_configuration->unhealthy_connections = 0;
        runtime_configuration->outstanding_requests = 0;
    }

    memset (runtime_configuration->reserved, 0, sizeof (runtime_configuration->reserved));
}

static int write_runtime_configuration (const int control_device_fd, struct kext_runtime_config * runtime_configuration)
{
    if (runtime_configuration->set_mask == 0)
        return USFS_SUCCESS;

    if (ioctl (control_device_fd, USFS_IOC_SET_RUNTIME_CONFIG, runtime_configuration) != 0)
    {
        fprintf (stderr, "Failed to write runtime configuration to device descriptor %d: %s\n", control_device_fd, strerror (errno));
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

static int refresh_runtime_configuration (const int control_device_fd, struct kext_runtime_config * runtime_configuration)
{
    if (runtime_configuration->set_mask == 0)
        return USFS_SUCCESS;

    if (ioctl (control_device_fd, USFS_IOC_GET_RUNTIME_CONFIG, runtime_configuration) != 0)
    {
        fprintf (stderr, "Failed to reread runtime configuration from device descriptor %d: %s\n", control_device_fd, strerror (errno));
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

static int execute_runtime_configuration_command (const int control_device_fd, const struct change_command_options * command_options)
{
    struct kext_runtime_config runtime_configuration;

    if (read_runtime_configuration (control_device_fd, &runtime_configuration) != USFS_SUCCESS)
        return USFS_FAILURE;

    if (apply_attribute_assignments (&runtime_configuration, command_options) != USFS_SUCCESS)
        return USFS_FAILURE;

    prepare_runtime_configuration (&runtime_configuration);

    if (write_runtime_configuration (control_device_fd, &runtime_configuration) != USFS_SUCCESS)
        return USFS_FAILURE;

    if (refresh_runtime_configuration (control_device_fd, &runtime_configuration) != USFS_SUCCESS)
        return USFS_FAILURE;

    print_runtime_configuration (&runtime_configuration);
    return USFS_SUCCESS;
}

static int execute_configuration_command_on_device (const struct change_command_options * command_options)
{
    const char * logical_device_name = command_options->logical_device_name;
    const int control_device_fd = open_control_device (logical_device_name);

    if (control_device_fd < 0)
        return USFS_FAILURE;

    const int command_result = execute_runtime_configuration_command (control_device_fd, command_options);

    if (close (control_device_fd) != 0)
    {
        fprintf (stderr, "Failed to close device file /dev/%s: %s\n", logical_device_name, strerror (errno));
        return USFS_FAILURE;
    }

    return command_result;
}

static int process_change_command (const struct change_command_options * command_options)
{
    const int configuration_lock_id = usfs_config_lock_acquire ();

    if (configuration_lock_id == -1)
    {
        fprintf (stderr, "Failed to acquire configuration lock for device %s: see %s for transaction details\n", command_options->logical_device_name, USFS_LOG_PATH);
        return USFS_FAILURE;
    }

    const int command_result = execute_configuration_command_on_device (command_options);

    if (usfs_config_lock_release (configuration_lock_id) != USFS_SUCCESS)
    {
        fprintf (stderr, "Failed to release configuration lock %d for device %s: see %s for transaction details\n", configuration_lock_id, command_options->logical_device_name, USFS_LOG_PATH);
        return USFS_FAILURE;
    }

    return command_result;
}

int main (const int argc, char ** argv)
{
    struct change_command_options command_options = { 0 };

    if (parse_command_options (argc, argv, &command_options) != USFS_SUCCESS)
        return USFS_FAILURE;

    return process_change_command (&command_options);
}

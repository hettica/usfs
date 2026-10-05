// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "usfs_common.h"
#include "usfs_config_instrumentation.h"

/* Shared implementation for the AIX ODM configuration methods. */

/* ------------------------------------------------------------------------- *
 * Compile-time configuration
 * ------------------------------------------------------------------------- */

const char * USFS_DRIVER_NAME = "usfs";
const char * USFS_KEXT_PATH = "/usr/lib/drivers/usfs/usfs.kext";
const char * USFS_LOG_PATH = "/var/log/usfsctl";

/* ODM customized database and its configuration lock. */
static const char * USFS_ODM_CUSTOMIZED_PATH = "/etc/objrepos";
static const char * USFS_ODM_LOCK_FILE = "/etc/objrepos/config_lock";

/* ------------------------------------------------------------------------- *
 * Tracing
 * ------------------------------------------------------------------------- */

void usfs_log (const char * message)
{
    FILE * log_file = fopen (USFS_LOG_PATH, "a");
    if (log_file == NULL)
    {
        return;
    }

    fputs (message, log_file);
    fclose (log_file);
}

/* ------------------------------------------------------------------------- *
 * ODM error reporting
 * ------------------------------------------------------------------------- */

/* Reserved ODM error hook; recognized errors currently produce no output. */
static void log_odm_error (const int error_code)
{
    switch (error_code)
    {
        case ODMI_INVALID_PATH:
            break;
        case ODMI_MALLOC_ERR:
            break;
        case ODMI_BAD_LOCK:
            break;
        case ODMI_BAD_TIMEOUT:
            break;
        case ODMI_BAD_TOKEN:
            break;
        case ODMI_LOCK_BLOCKED:
            break;
        case ODMI_LOCK_ENV:
            break;
        case ODMI_UNLOCK:
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------------- *
 * Kernel extension lifecycle
 * ------------------------------------------------------------------------- */

/* Builds the cfg_load query that identifies the extension by its path. */
static struct cfg_load usfs_kext_make_query (void)
{
    static char empty_libpath[] = "";
    struct cfg_load query;
    memset (&query, 0, sizeof (query));

    query.path = (char *)USFS_KEXT_PATH;
    query.libpath = empty_libpath;

    return query;
}

/*
 * Runs SYS_QUERYLOAD for the extension. On USFS_SUCCESS the module id
 * (USFS_MODULE_ID_NONE when the object is not loaded) is stored in
 * *module_id. sysconfig() reports failure by returning -1 with errno set.
 */
static int query_kernel_module (mid_t * module_id)
{
    struct cfg_load query = usfs_kext_make_query ();

    if (sysconfig (SYS_QUERYLOAD, &query, sizeof (query)) != 0)
    {
        return USFS_FAILURE;
    }

    *module_id = query.kmid;
    return USFS_SUCCESS;
}

mid_t usfs_kext_get_module_id (void)
{
    mid_t module_id = USFS_MODULE_ID_NONE;

    if (query_kernel_module (&module_id) != USFS_SUCCESS)
    {
        return USFS_MODULE_ID_NONE;
    }

    return module_id;
}

int usfs_kext_is_loaded (void)
{
    mid_t module_id = USFS_MODULE_ID_NONE;

    if (query_kernel_module (&module_id) != USFS_SUCCESS)
    {
        return USFS_QUERY_ERROR;
    }

    return module_id == USFS_MODULE_ID_NONE ? USFS_QUERY_ABSENT : USFS_QUERY_PRESENT;
}

int usfs_kext_load (void)
{
    struct cfg_load query = usfs_kext_make_query ();

    if (sysconfig (SYS_KLOAD, &query, sizeof (query)) != 0)
    {
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

/* ------------------------------------------------------------------------- *
 * File system type registration (/etc/vfs)
 *
 * /etc/vfs maps a file system type name to the type number the kernel knows
 * it by, plus the helper programs mount(8) should run. Without an entry, the
 * mount, umount and df commands refuse to operate on a mounted USFS file
 * system ("has a gfstype N that is not known"), even though the mount itself
 * works: USFS daemons mount themselves by calling vmount() directly and never
 * go through a mount helper.
 *
 * Entry format (whitespace separated):
 *
 *     %name  %vfsnum  %mount_helper  %fs_helper  [%options]
 *
 * The type number is assigned dynamically by the kernel extension, which
 * probes for a free slot when it registers the file system, so it cannot be
 * hardcoded here; it is read back from the configured device through the
 * USFS_IOC_CONNECTION_REQUEST.
 * ------------------------------------------------------------------------- */

static const char * USFS_VFS_FILE = "/etc/vfs";

/* Defined further down, with the other device special file helpers. */
static int make_device_path (const char * logical_device_name, char * device_path, const size_t path_capacity);

/* True when the line's first field is exactly the USFS type name. Comment and
 * blank lines never match. */
static int is_usfs_vfs_line (const char * line)
{
    const size_t driver_name_length = strlen (USFS_DRIVER_NAME);
    size_t character_index = 0;

    while (line[character_index] == ' ' || line[character_index] == '\t')
    {
        character_index += 1;
    }

    if (line[character_index] == '#' || line[character_index] == '\0' || line[character_index] == '\n')
    {
        return false;
    }

    if (strncmp (line + character_index, USFS_DRIVER_NAME, driver_name_length) != 0)
    {
        return false;
    }

    const char delimiter = line[character_index + driver_name_length];
    if (delimiter == ' ' || delimiter == '\t')
        return true;

    return delimiter == '\n' || delimiter == '\0';
}

#include "vfs_text.h"

int usfs_type_is_registered (void)
{
    FILE * vfs_file = fopen (USFS_VFS_FILE, "r");
    if (vfs_file == NULL)
    {
        /* A missing /etc/vfs is not an error here: the entry is simply absent
         * and usfs_type_register creates the file. */
        if (errno == ENOENT)
        {
            return USFS_QUERY_ABSENT;
        }

        return USFS_QUERY_ERROR;
    }

    char line[USFS_LOG_BUFFER_SIZE];
    int found = false;

    int line_rc;
    while ((line_rc = usfs_vfs_read_line (vfs_file, line, sizeof (line))) > 0)
    {
        if (is_usfs_vfs_line (line))
        {
            found = true;
        }
    }

    const int failed = line_rc < 0 || ferror (vfs_file);
    if (fclose (vfs_file) != 0 || failed)
        return USFS_QUERY_ERROR;

    return found ? USFS_QUERY_PRESENT : USFS_QUERY_ABSENT;
}

/*
 * Reads the file system type number the kernel extension registered, by
 * asking the configured device. Returns USFS_SUCCESS and stores the number in
 * *gfs_type on success.
 */
static int query_gfs_type (const char * logical_device_name, int * gfs_type)
{
    char device_path[USFS_DEVICE_PATH_MAX];
    if (make_device_path (logical_device_name, device_path, sizeof (device_path)) != USFS_SUCCESS)
    {
        return USFS_FAILURE;
    }

    const int device_descriptor = open (device_path, O_RDWR);
    if (device_descriptor < 0)
    {
        return USFS_FAILURE;
    }

    struct usfs_dev_info device_info;
    memset (&device_info, 0, sizeof (device_info));

    const int rc = ioctl (device_descriptor, USFS_IOC_CONNECTION_REQUEST, &device_info);
    close (device_descriptor);

    if (rc != 0)
    {
        return USFS_FAILURE;
    }

    if (device_info.protocol_version != USFS_PROTOCOL_VERSION)
    {
        return USFS_FAILURE;
    }

    *gfs_type = (int)device_info.fs_type;
    return USFS_SUCCESS;
}

static FILE * create_vfs_temporary (char * temporary_path, const size_t capacity)
{
    if (snprintf (temporary_path, capacity, "%s.usfs.XXXXXX", USFS_VFS_FILE) >=
        (int)capacity)
    {
        return NULL;
    }

    const int descriptor = mkstemp (temporary_path);
    FILE * stream = descriptor < 0 ? NULL : fdopen (descriptor, "w");
    if (stream == NULL && descriptor >= 0)
    {
        close (descriptor);
        unlink (temporary_path);
    }

    return stream;
}

static int copy_other_vfs_entries (FILE * source, FILE * target, int * last_character)
{
    char line[USFS_LOG_BUFFER_SIZE];
    int line_rc = 0;
    int failed = false;

    while (source != NULL &&
           (line_rc = usfs_vfs_read_line (source, line, sizeof (line))) > 0)
    {
        if (is_usfs_vfs_line (line))
            continue;
        if (fputs (line, target) == EOF)
        {
            failed = true;
            break;
        }
        if (line[0] != '\0')
            *last_character = line[strlen (line) - 1];
    }

    if (failed)
        return USFS_FAILURE;

    if (line_rc < 0)
        return USFS_FAILURE;

    if (source != NULL && ferror (source))
        return USFS_FAILURE;

    return USFS_SUCCESS;
}

static int write_vfs_registration (FILE * target, const int last_character, const int file_system_type)
{
    return fprintf (target, "%s%s\t\t%d\tnone\t\t\tnone\n", last_character == '\n' ? "" : "\n", USFS_DRIVER_NAME, file_system_type) < 0
               ? USFS_FAILURE
               : USFS_SUCCESS;
}

static int finish_vfs_temporary (FILE * target, const char * temporary_path, const struct stat * source_metadata, const int prior_failure)
{
    int failed = prior_failure;

    if (!failed && fflush (target) != 0)
        failed = true;
    if (!failed && source_metadata != NULL &&
        chown (temporary_path, source_metadata->st_uid, source_metadata->st_gid) != 0)
        failed = true;
    if (!failed &&
        chmod (temporary_path, source_metadata != NULL ? source_metadata->st_mode & 07777 : 0644) != 0)
        failed = true;
    if (!failed && fsync (fileno (target)) != 0)
        failed = true;
    if (fclose (target) != 0)
        failed = true;

    return failed ? USFS_FAILURE : USFS_SUCCESS;
}

static int write_replacement_vfs (FILE * source, FILE * target, const char * temporary_path, const struct stat * source_metadata, const int register_type, const int file_system_type)
{
    int last_character = '\n';
    int failed = false;

    if (copy_other_vfs_entries (source, target, &last_character) !=
        USFS_SUCCESS)
        failed = true;
    if (source != NULL)
    {
        if (fclose (source) != 0)
            failed = true;
    }

    if (!failed && register_type &&
        write_vfs_registration (target, last_character, file_system_type) != USFS_SUCCESS)
        failed = true;
    failed = finish_vfs_temporary (target, temporary_path, source_metadata, failed) != USFS_SUCCESS;
    if (failed || rename (temporary_path, USFS_VFS_FILE) != 0)
    {
        unlink (temporary_path);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

static int rewrite_vfs (const int register_type, const int file_system_type)
{
    char temporary_path[USFS_DEVICE_PATH_MAX];
    struct stat source_metadata;
    FILE * source = fopen (USFS_VFS_FILE, "r");
    const int source_errno = source == NULL ? errno : 0;
    const int source_exists = source != NULL;

    if (source == NULL)
    {
        if (source_errno == ENOENT && !register_type)
            return USFS_SUCCESS;
        if (source_errno != ENOENT)
            return USFS_FAILURE;
    }

    if (source_exists && (fstat (fileno (source), &source_metadata) != 0 ||
                          !S_ISREG (source_metadata.st_mode)))
    {
        fclose (source);
        return USFS_FAILURE;
    }

    FILE * target = create_vfs_temporary (temporary_path, sizeof (temporary_path));
    if (target == NULL)
    {
        if (source != NULL)
            fclose (source);
        return USFS_FAILURE;
    }

    return write_replacement_vfs (source, target, temporary_path, source_exists ? &source_metadata : NULL, register_type, file_system_type);
}

int usfs_type_register (const char * logical_device_name)
{
    int gfs_type = 0;
    if (query_gfs_type (logical_device_name, &gfs_type) != USFS_SUCCESS)
        return USFS_FAILURE;

    return rewrite_vfs (true, gfs_type);
}

int usfs_type_unregister (void)
{
    return rewrite_vfs (false, 0);
}

/* ------------------------------------------------------------------------- *
 * Device major/minor numbers
 * ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- *
 * Device special file (/dev node)
 * ------------------------------------------------------------------------- */

/* Formats "/dev/<name>" into device_path. Returns USFS_FAILURE if it would not fit. */
static int make_device_path (const char * logical_device_name, char * device_path, const size_t path_capacity)
{
    if (logical_device_name == NULL)
        return USFS_FAILURE;

    if (strcmp (logical_device_name, USFS_LOGICAL_DEVICE_NAME) != 0)
        return USFS_FAILURE;

    const int written = snprintf (device_path, path_capacity, "/dev/%s", logical_device_name);
    if (written < 0 || (size_t)written >= path_capacity)
    {
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

/* Validates /dev/<name> ownership when present; absence is acceptable. */
static int usfs_validate_device_node (const char * logical_device_name, const dev_t device_number, const int number_state)
{
    char device_path[USFS_DEVICE_PATH_MAX];
    if (make_device_path (logical_device_name, device_path, sizeof (device_path)) != USFS_SUCCESS)
    {
        return USFS_FAILURE;
    }

    struct stat device_metadata;
    if (lstat (device_path, &device_metadata) != 0)
    {
        return errno == ENOENT ? USFS_SUCCESS : USFS_FAILURE;
    }

    if (number_state != USFS_QUERY_PRESENT)
        return USFS_FAILURE;

    if (!S_ISCHR (device_metadata.st_mode))
        return USFS_FAILURE;

    if (device_metadata.st_rdev != device_number)
        return USFS_FAILURE;

    return USFS_SUCCESS;
}

static int usfs_remove_device_node (const char * logical_device_name, const dev_t device_number, const int number_state)
{
    char device_path[USFS_DEVICE_PATH_MAX];
    if (usfs_validate_device_node (logical_device_name, device_number, number_state) != USFS_SUCCESS)
        return USFS_FAILURE;

    if (make_device_path (logical_device_name, device_path, sizeof (device_path)) != USFS_SUCCESS)
        return USFS_FAILURE;

    if (unlink (device_path) == 0)
        return USFS_SUCCESS;

    return errno == ENOENT ? USFS_SUCCESS : USFS_FAILURE;
}

static int apply_device_node_policy (const char * device_path)
{
    /* mknod(2) is affected by the invoking process's umask.  Apply owner,
     * group, and mode explicitly so configuration never inherits ambient
     * security state.  Remove the node if the complete policy cannot be
     * established; configure rollback will handle the remaining state. */
    /* This locked, single-threaded ODM method owns both static results. */
    /* cppcheck-suppress getpwnamCalled */
    const struct passwd * owner = getpwnam (USFS_DEVICE_NODE_OWNER);
    /* cppcheck-suppress getgrnamCalled */
    const struct group * group = getgrnam (USFS_DEVICE_NODE_GROUP);
    if (owner == NULL || group == NULL ||
        chown (device_path, owner->pw_uid, group->gr_gid) != 0 ||
        chmod (device_path, USFS_DEVICE_NODE_PERMS) != 0)
    {
        (void)unlink (device_path);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

/*
 * Creates the character special file /dev/<name> for device_number, replacing
 * any stale node so the operation is idempotent.
 */
static int usfs_create_device_node (const char * logical_device_name, const dev_t device_number)
{
    char device_path[USFS_DEVICE_PATH_MAX];
    if (make_device_path (logical_device_name, device_path, sizeof (device_path)) != USFS_SUCCESS)
    {
        return USFS_FAILURE;
    }

    if (usfs_remove_device_node (logical_device_name, device_number, USFS_QUERY_PRESENT) != USFS_SUCCESS)
    {
        return USFS_FAILURE;
    }

    if (mknod (device_path, S_IFCHR | USFS_DEVICE_NODE_PERMS, device_number) != 0)
    {
        return USFS_FAILURE;
    }

    return apply_device_node_policy (device_path);
}

/* ------------------------------------------------------------------------- *
 * ODM device status (CuDv.status)
 * ------------------------------------------------------------------------- */

/* Opens an ODM session against the customized database. Returns USFS_SUCCESS. */
static int open_odm_session (void)
{
    /* When ODMDIR is unset, target the customized DB explicitly. */
    if (getenv ("ODMDIR") == NULL)
    {
        char * previous_path = odm_set_path ((char *)USFS_ODM_CUSTOMIZED_PATH);
        if (previous_path == (char *)-1)
        {
            log_odm_error (odmerrno);
            return USFS_FAILURE;
        }
        free (previous_path);
    }

    if (odm_initialize () == -1)
    {
        log_odm_error (odmerrno);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

/* Closes the ODM session opened by open_odm_session (). Returns USFS_SUCCESS. */
static int close_odm_session (void)
{
    if (odm_terminate () == -1)
    {
        log_odm_error (odmerrno);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

int usfs_config_lock_acquire (void)
{
    if (open_odm_session () != USFS_SUCCESS)
        return -1;

    const int lock_id = odm_lock ((char *)USFS_ODM_LOCK_FILE, ODM_WAIT);
    if (lock_id == -1)
    {
        log_odm_error (odmerrno);
        (void)close_odm_session ();
        return -1;
    }

    if (usfs_config_instrumentation_checkpoint (
            USFS_CONFIG_METHOD_LOCKED
        ) != USFS_SUCCESS)
    {
        (void)odm_unlock (lock_id);
        (void)close_odm_session ();
        return -1;
    }

    return lock_id;
}

int usfs_config_lock_release (const int lock_id)
{
    int rc = USFS_SUCCESS;

    if (odm_unlock (lock_id) == -1)
    {
        log_odm_error (odmerrno);
        rc = USFS_FAILURE;
    }

    if (close_odm_session () != USFS_SUCCESS)
        rc = USFS_FAILURE;
    return rc;
}

/*
 * Applies the status change to the CuDv object for <logical_device_name>.
 * Requires an open ODM session with the configuration lock held.
 */
enum usfs_missing_status_action
{
    USFS_KEEP_MISSING_STATUS,
    USFS_CLEAR_MISSING_STATUS
};

static int odm_write_device_status (const char * logical_device_name, const short new_status, const enum usfs_missing_status_action action)
{
    if (logical_device_name == NULL)
        return USFS_FAILURE;

    if (strcmp (logical_device_name, USFS_LOGICAL_DEVICE_NAME) != 0)
        return USFS_FAILURE;

    char criteria[USFS_ODM_QUERY_MAX];
    snprintf (criteria, sizeof (criteria), "name=\"%s\"", logical_device_name);

    struct CuDv device_record;
    memset (&device_record, 0, sizeof (device_record));

    if (odm_get_obj (CuDv_CLASS, criteria, &device_record, ODM_FIRST) == (void *)-1)
    {
        log_odm_error (odmerrno);
        return USFS_FAILURE;
    }

    if (strcmp (device_record.name, logical_device_name) != 0 ||
        strcmp (device_record.PdDvLn_Lvalue, USFS_DEVICE_UNIQUE_TYPE) != 0)
    {
        return USFS_FAILURE;
    }

    device_record.status = new_status;
    if (action == USFS_CLEAR_MISSING_STATUS && device_record.chgstatus == MISSING)
    {
        device_record.chgstatus = SAME;
    }

    if (odm_change_obj (CuDv_CLASS, &device_record) == -1)
    {
        log_odm_error (odmerrno);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

/*
 * Sets CuDv.status for <logical_device_name> to new_status (AVAILABLE or
 * DEFINED). The caller owns the method-wide ODM configuration lock. When
 * transitioning to AVAILABLE a MISSING change-status is reset to SAME.
 */
static int set_device_status (const char * logical_device_name, const short new_status, const enum usfs_missing_status_action action)
{
    return odm_write_device_status (logical_device_name, new_status, action);
}

static int usfs_mark_device_available (const char * logical_device_name)
{
    if (usfs_config_instrumentation_checkpoint (
            USFS_CONFIG_METHOD_MARK_AVAILABLE
        ) != USFS_SUCCESS)
        return USFS_FAILURE;

    return set_device_status (logical_device_name, AVAILABLE, USFS_CLEAR_MISSING_STATUS);
}

static int usfs_mark_device_defined (const char * logical_device_name)
{
    return set_device_status (logical_device_name, DEFINED, USFS_KEEP_MISSING_STATUS);
}

static int usfs_device_is_available (const char * logical_device_name)
{
    char criteria[USFS_ODM_QUERY_MAX];
    struct CuDv device_record;

    if (logical_device_name == NULL)
        return USFS_QUERY_ERROR;

    if (strcmp (logical_device_name, USFS_LOGICAL_DEVICE_NAME) != 0)
        return USFS_QUERY_ERROR;

    snprintf (criteria, sizeof (criteria), "name=\"%s\"", logical_device_name);
    memset (&device_record, 0, sizeof (device_record));
    if (odm_get_obj (CuDv_CLASS, criteria, &device_record, ODM_FIRST) == (void *)-1)
        return USFS_QUERY_ERROR;

    if (strcmp (device_record.name, logical_device_name) != 0)
        return USFS_QUERY_ERROR;

    if (strcmp (device_record.PdDvLn_Lvalue, USFS_DEVICE_UNIQUE_TYPE) != 0)
        return USFS_QUERY_ERROR;

    return device_record.status == AVAILABLE ? USFS_QUERY_PRESENT : USFS_QUERY_ABSENT;
}

/* ------------------------------------------------------------------------- *
 * Device switch configuration (SYS_CFGDD)
 * ------------------------------------------------------------------------- */

static int is_coverage_name_character (const unsigned char character)
{
    if (character >= 'A' && character <= 'Z')
        return true;

    if (character >= 'a' && character <= 'z')
        return true;

    if (character >= '0' && character <= '9')
        return true;

    if (character == '.' || character == '_' || character == '-')
        return true;

    return false;
}

static int is_coverage_name_valid (const char * name)
{
    if (name == NULL)
        return false;

    if (*name == '\0')
        return false;

    const unsigned char * cursor = (const unsigned char *)name;
    for (; *cursor != '\0'; cursor++)
    {
        if (!is_coverage_name_character (*cursor))
            return false;
    }

    return true;
}

static int read_coverage_number (const char * generation_path, uint32_t * generation_number)
{
    unsigned long parsed_number;
    FILE * generation_file = fopen (generation_path, "r");
    if (generation_file == NULL)
        return USFS_FAILURE;

    if (fscanf (generation_file, "%lu", &parsed_number) != 1 ||
        parsed_number == 0 || parsed_number > 0xffffffffUL)
    {
        fclose (generation_file);
        return USFS_FAILURE;
    }

    if (fclose (generation_file) != 0)
        return USFS_FAILURE;

    *generation_number = (uint32_t)parsed_number;
    return USFS_SUCCESS;
}

static int write_coverage_number (const char * generation_path, const uint32_t generation_number)
{
    char temporary_path[512];
    if (snprintf (temporary_path, sizeof (temporary_path), "%s.tmp.%ld", generation_path, (long)getpid ()) >= (int)sizeof (temporary_path))
        return USFS_FAILURE;

    FILE * generation_file = fopen (temporary_path, "w");
    if (generation_file == NULL)
        return USFS_FAILURE;

    if (fprintf (generation_file, "%u\n", generation_number) < 0 || fclose (generation_file) != 0)
    {
        unlink (temporary_path);
        return USFS_FAILURE;
    }

    if (rename (temporary_path, generation_path) != 0)
    {
        unlink (temporary_path);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

static int read_coverage_generation (const char * coverage_run, const int command, uint32_t * generation)
{
    const char * binary_directory = getenv ("USFS_BINARY_DIR");
    char current_generation_path[512];
    char next_generation_path[512];
    uint32_t generation_number;

    if (binary_directory == NULL)
        return USFS_FAILURE;

    if (binary_directory[0] != '/')
        return USFS_FAILURE;

    if (snprintf (current_generation_path, sizeof (current_generation_path), "%s/coverage/kernel-data/%s/current-generation", binary_directory, coverage_run) >= (int)sizeof (current_generation_path))
        return USFS_FAILURE;

    if (snprintf (next_generation_path, sizeof (next_generation_path), "%s/coverage/kernel-data/%s/next-generation", binary_directory, coverage_run) >= (int)sizeof (next_generation_path))
        return USFS_FAILURE;

    if (command == USFS_CFG_INIT)
    {
        if (read_coverage_number (next_generation_path, &generation_number) != USFS_SUCCESS)
            return USFS_FAILURE;

        if (generation_number == 0xffffffffu)
            return USFS_FAILURE;

        if (write_coverage_number (next_generation_path, generation_number + 1) != USFS_SUCCESS)
            return USFS_FAILURE;

        if (write_coverage_number (current_generation_path, generation_number) != USFS_SUCCESS)
            return USFS_FAILURE;
        *generation = generation_number;
        return USFS_SUCCESS;
    }

    return read_coverage_number (current_generation_path, generation);
}

static void clear_coverage_generation (const char * coverage_run)
{
    const char * binary_directory = getenv ("USFS_BINARY_DIR");
    char current_generation_path[512];

    if (binary_directory != NULL && binary_directory[0] == '/' &&
        snprintf (current_generation_path, sizeof (current_generation_path), "%s/coverage/kernel-data/%s/current-generation", binary_directory, coverage_run) < (int)sizeof (current_generation_path))
        unlink (current_generation_path);
}

static int write_coverage_bytes (const int output_descriptor, const void * buffer, unsigned remaining_size)
{
    const unsigned char * cursor = buffer;

    while (remaining_size != 0)
    {
        const ssize_t written = write (output_descriptor, cursor, remaining_size);
        if (written <= 0)
            return USFS_FAILURE;
        cursor += written;
        remaining_size -= (unsigned)written;
    }

    return USFS_SUCCESS;
}

struct coverage_export_paths
{
    char lifecycle_directory[512];      // Directory containing lifecycle snapshots.
    char container_path[768];           // Published binary snapshot path.
    char temporary_container_path[768]; // Temporary binary snapshot path.
    char metadata_path[768];            // Published command metadata path.
    char temporary_metadata_path[768];  // Temporary command metadata path.
};

static int validate_coverage_header (const struct usfs_gcov_container_header * header)
{
    if (header->magic != USFS_GCOV_MAGIC)
        return USFS_FAILURE;

    if (header->abi_version != USFS_GCOV_ABI_VERSION)
        return USFS_FAILURE;

    if (header->gcov_version != USFS_GCOV_VERSION)
        return USFS_FAILURE;

    if (header->unit_count == 0)
        return USFS_FAILURE;

    if (header->total_size < sizeof (*header))
        return USFS_FAILURE;

    if (header->total_size > USFS_GCOV_MAX_SNAPSHOT)
        return USFS_FAILURE;

    return USFS_SUCCESS;
}

static int prepare_coverage_export_paths (const char * binary_directory, const char * coverage_run, const char * coverage_event, const uint32_t generation, struct coverage_export_paths * paths)
{
    if (binary_directory == NULL)
        return USFS_FAILURE;

    if (binary_directory[0] != '/')
        return USFS_FAILURE;

    if (snprintf (paths->lifecycle_directory, sizeof (paths->lifecycle_directory), "%s/coverage/kernel-data/%s/lifecycle", binary_directory, coverage_run) >=
        (int)sizeof (paths->lifecycle_directory))
        return USFS_FAILURE;

    if (snprintf (paths->container_path, sizeof (paths->container_path), "%s/%08u-%s-%ld.container", paths->lifecycle_directory, generation, coverage_event, (long)getpid ()) >=
        (int)sizeof (paths->container_path))
        return USFS_FAILURE;

    if (snprintf (paths->temporary_container_path, sizeof (paths->temporary_container_path), "%s.tmp", paths->container_path) >=
        (int)sizeof (paths->temporary_container_path))
        return USFS_FAILURE;

    if (snprintf (paths->metadata_path, sizeof (paths->metadata_path), "%s.meta", paths->container_path) >=
        (int)sizeof (paths->metadata_path))
        return USFS_FAILURE;

    if (snprintf (paths->temporary_metadata_path, sizeof (paths->temporary_metadata_path), "%s.tmp", paths->metadata_path) >= (int)sizeof (paths->temporary_metadata_path))
        return USFS_FAILURE;

    return USFS_SUCCESS;
}

static int write_coverage_container (const struct coverage_export_paths * paths, const void * buffer, const unsigned snapshot_size)
{
    const int output_descriptor = open (paths->temporary_container_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output_descriptor < 0)
        return USFS_FAILURE;

    if (write_coverage_bytes (output_descriptor, buffer, snapshot_size) !=
        USFS_SUCCESS)
    {
        close (output_descriptor);
        unlink (paths->temporary_container_path);
        return USFS_FAILURE;
    }

    if (close (output_descriptor) != 0 || rename (paths->temporary_container_path, paths->container_path) != 0)
    {
        unlink (paths->temporary_container_path);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

static int write_coverage_metadata (const struct coverage_export_paths * paths, const char * coverage_event, const uint32_t generation, const int original_command, const int configuration_rc, const int export_command, const int export_rc)
{
    FILE * metadata_file = fopen (paths->temporary_metadata_path, "w");
    if (metadata_file == NULL)
        return USFS_FAILURE;

    if (fprintf (metadata_file, "event=%s\ngeneration=%u\noriginal_cmd=%d\n"
                                "original_rc=%d\nexport_cmd=%d\nexport_rc=%d\n",
                 coverage_event,
                 generation,
                 original_command,
                 configuration_rc,
                 export_command,
                 export_rc) < 0 ||
        fclose (metadata_file) != 0)
    {
        unlink (paths->temporary_metadata_path);
        return USFS_FAILURE;
    }

    if (rename (paths->temporary_metadata_path, paths->metadata_path) != 0)
    {
        unlink (paths->temporary_metadata_path);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

static int store_coverage_export (const char * coverage_run, const char * coverage_event, const uint32_t generation, const int original_command, const int configuration_rc, const int export_command, const int export_rc, const void * buffer)
{
    const struct usfs_gcov_container_header * header = buffer;
    const char * binary_directory = getenv ("USFS_BINARY_DIR");
    struct coverage_export_paths paths;

    if (validate_coverage_header (header) != USFS_SUCCESS)
        return USFS_FAILURE;

    if (prepare_coverage_export_paths (binary_directory, coverage_run, coverage_event, generation, &paths) != USFS_SUCCESS)
        return USFS_FAILURE;

    if (write_coverage_container (&paths, buffer, header->total_size) != USFS_SUCCESS)
        return USFS_FAILURE;

    return write_coverage_metadata (&paths, coverage_event, generation, original_command, configuration_rc, export_command, export_rc);
}

static int request_coverage_export (const mid_t module_id, const dev_t device_number, const uint32_t generation, const int export_command, void * buffer)
{
    struct cfg_dd query;
    struct usfs_config_coverage_export request;

    memset (&request, 0, sizeof (request));
    request.magic = USFS_CONFIG_DDS_MAGIC;
    request.abi_version = USFS_CONFIG_ABI_VERSION;
    request.size = sizeof (request);
    request.generation = generation;
    request.capacity = USFS_GCOV_MAX_SNAPSHOT;
    request.user_buffer = (uint64_t)(unsigned long)buffer;

    memset (&query, 0, sizeof (query));
    query.kmid = module_id;
    query.cmd = export_command;
    query.devno = device_number;
    query.ddsptr = (caddr_t)&request;
    query.ddslen = sizeof (request);
    return sysconfig (SYS_CFGDD, &query, sizeof (query));
}

static int export_coverage_command (const mid_t module_id, const dev_t device_number, const char * coverage_run, const char * coverage_event, const uint32_t generation, const int original_command, const int configuration_rc, const int export_command)
{
    void * buffer = malloc (USFS_GCOV_MAX_SNAPSHOT);
    if (buffer == NULL)
        return USFS_FAILURE;

    memset (buffer, 0, USFS_GCOV_MAX_SNAPSHOT);
    const int export_rc = request_coverage_export (module_id, device_number, generation, export_command, buffer);

    /*
     * A deliberately failed final unpin still returns its errno after copying
     * a valid snapshot. Preserve that evidence before propagating failure.
     */
    const int store_rc = store_coverage_export (
        coverage_run,
        coverage_event,
        generation,
        original_command,
        configuration_rc,
        export_command,
        export_rc,
        buffer
    );
    free (buffer);
    if (store_rc != USFS_SUCCESS || export_rc != 0)
        return USFS_FAILURE;

    return USFS_SUCCESS;
}

static int export_configuration_coverage (const mid_t module_id, const dev_t device_number, const char * coverage_run, const char * coverage_event, const uint32_t generation, const int command, const int configuration_rc)
{
    char automatic_event[96];
    const int export_command =
        (command == USFS_CFG_INIT || configuration_rc == 0) ? USFS_CFG_INSTRUMENTATION_DRAIN : USFS_CFG_INSTRUMENTATION_EXPORT;
    if (!is_coverage_name_valid (coverage_event))
    {
        snprintf (automatic_event, sizeof (automatic_event), "unplanned-%s-%ld", command == USFS_CFG_INIT ? "init" : "term", (long)getpid ());
        coverage_event = automatic_event;
    }

    if (export_coverage_command (
            module_id,
            device_number,
            coverage_run,
            coverage_event,
            generation,
            command,
            configuration_rc,
            export_command
        ) != USFS_SUCCESS)
        return USFS_FAILURE;

    if (export_command == USFS_CFG_INSTRUMENTATION_DRAIN)
        clear_coverage_generation (coverage_run);

    return USFS_SUCCESS;
}

static int prepare_device_configuration (struct cfg_dd * query, struct usfs_dev_cfg * configuration, const mid_t module_id, const dev_t device_number, const int command, const int coverage_active, const char * coverage_run, uint32_t * generation)
{
    memset (query, 0, sizeof (*query));
    memset (configuration, 0, sizeof (*configuration));
    query->kmid = module_id;
    query->cmd = command;
    query->devno = device_number;
    configuration->magic = USFS_CONFIG_DDS_MAGIC;
    configuration->abi_version = USFS_CONFIG_ABI_VERSION;
    configuration->size = sizeof (*configuration);
    if (coverage_active)
    {
        if (read_coverage_generation (coverage_run, command, generation) !=
            USFS_SUCCESS)
            return USFS_FAILURE;
        configuration->flags |= USFS_CONFIG_FLAG_DEFERRED_EXPORT;
        configuration->generation = *generation;
    }
    usfs_config_instrumentation_prepare (
        configuration,
        command == USFS_CFG_INIT ? USFS_CONFIG_OPERATION_INITIALIZE : USFS_CONFIG_OPERATION_TERMINATE
    );
    if (configuration->flags != 0)
    {
        query->ddsptr = (caddr_t)configuration;
        query->ddslen = sizeof (*configuration);
    }

    return USFS_SUCCESS;
}

/* Sends a USFS_CFG_INIT/USFS_CFG_TERM configuration command. Returns USFS_SUCCESS. */
static int usfs_devsw_send_command (const mid_t module_id, const dev_t device_number, const int command)
{
    struct cfg_dd query;
    struct usfs_dev_cfg configuration;
    const char * coverage_run = getenv ("USFS_COVERAGE_SUITE");
    const char * coverage_event = getenv ("USFS_COVERAGE_EVENT");
    const int coverage_active =
        is_coverage_name_valid (coverage_run) &&
        access ("/usr/sbin/usfs_coverage", X_OK) == 0;
    uint32_t generation = 0;
    if (prepare_device_configuration (&query, &configuration, module_id, device_number, command, coverage_active, coverage_run, &generation) != USFS_SUCCESS)
        return USFS_FAILURE;

    const int configuration_rc = sysconfig (SYS_CFGDD, &query, sizeof (query));
    if (configuration_rc != 0)
        USFS_LOGF ("SYS_CFGDD command=%d failed errno=%d", command, errno);
    if (coverage_active &&
        ((command == USFS_CFG_INIT && configuration_rc != 0) ||
         command == USFS_CFG_TERM))
    {
        if (export_configuration_coverage (module_id, device_number, coverage_run, coverage_event, generation, command, configuration_rc) != USFS_SUCCESS)
            return USFS_FAILURE;
    }

    if (configuration_rc != 0)
    {
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

#include "configuration_transactions.h"

/* ------------------------------------------------------------------------- *
 * Argument parsing
 * ------------------------------------------------------------------------- */

int usfs_parse_device_arg (const int argc, char ** argv, const char ** logical_device_name)
{
    /* Expected invocation: <tool> -l <logical_device_name> */
    const int expected_argc = 3;

    if (argc != expected_argc)
    {
        return USFS_FAILURE;
    }

    if (strcmp (argv[1], "-l") != 0)
        return USFS_FAILURE;

    if (strcmp (argv[2], USFS_LOGICAL_DEVICE_NAME) != 0)
        return USFS_FAILURE;

    *logical_device_name = argv[2];
    return USFS_SUCCESS;
}

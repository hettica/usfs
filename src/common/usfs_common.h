// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_COMMON_H
#define USFS_COMMON_H

/*
 * Shared building blocks for the USFS device configuration
 * methods (cfgusfs, ucfgusfs, chusfs).
 *
 * These methods are invoked by the AIX Object Data Manager (ODM) device
 * configuration framework:
 *
 *     mkdev -l usfs0            -> Configure   method: cfgusfs  -l usfs0
 *     chdev -l usfs0 -a k=v     -> Change      method: chusfs   -l usfs0 -a k=v
 *     rmdev -l usfs0            -> Unconfigure method: ucfgusfs -l usfs0
 *
 * The tool-specific sources contain only argument handling and the top-level
 * call into this module; everything that more than one method needs lives
 * here so the behaviour stays identical across tools.
 *
 * Return-code convention for functions in this module (unless documented
 * otherwise): 0 on success, non-zero on failure. Tri-state query helpers
 * document their own {-1, 0, 1} meaning inline.
 */

#include <sys/vfs.h>
#include <sys/gfs.h>
#include <sys/vnode.h>
#include <sys/vmount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysconfig.h>
#include <sys/device.h>
#include <sys/sysmacros.h>
#include <sys/cfgodm.h>
#include <sys/cfgdb.h>

#include <sys/ioctl.h>

#include <cf.h>
#include <odmi.h>
#include <grp.h>
#include <pwd.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>

/* Wire protocol shared with the kernel extension: USFS_IOC_CONNECTION_REQUEST is used to
 * read back the dynamically assigned file system type number. */
#include "usfs_proto.h"
#include "usfs_config.h"
#include "usfs_gcov.h"

/* ------------------------------------------------------------------------- *
 * Compile-time configuration
 * ------------------------------------------------------------------------- */

/* ODM device driver name (PdDv.DvDr / genmajor key) for this pseudo-device. */
extern const char * USFS_DRIVER_NAME;

/* On-disk location of the loadable kernel extension. */
extern const char * USFS_KEXT_PATH;

/* Append-only trace log written by the configuration methods. */
extern const char * USFS_LOG_PATH;

/* Largest single formatted trace line, including the terminating NUL. */
#define USFS_LOG_BUFFER_SIZE 4096

/* Upper bound for a "/dev/<name>" special-file path. */
#define USFS_DEVICE_PATH_MAX 256

/* Upper bound for an ODM query predicate such as name="usfs0". */
#define USFS_ODM_QUERY_MAX 256

/* Deliberate control-device policy.  Only root and members of AIX's
 * administrative system group may create daemon channels. */
#define USFS_DEVICE_NODE_OWNER   "root"
#define USFS_DEVICE_NODE_GROUP   "system"
#define USFS_DEVICE_NODE_PERMS   0660
#define USFS_LOGICAL_DEVICE_NAME "usfs0"
#define USFS_DEVICE_UNIQUE_TYPE  "cdr/fs/usfs"

/* ------------------------------------------------------------------------- *
 * Result codes
 *
 * Functions in this module use these in place of raw literals. main() also
 * returns USFS_SUCCESS / USFS_FAILURE (0 / non-zero), which is what the ODM
 * configuration framework expects from a device method.
 * ------------------------------------------------------------------------- */

/* Outcome of an action (configure, load, create, ...). */
#define USFS_SUCCESS 0
#define USFS_FAILURE 1

/* Outcome of a tri-state existence query (is-loaded, is-registered, ...). */
#define USFS_QUERY_ERROR   (-1) /* the query itself failed */
#define USFS_QUERY_ABSENT  0    /* queried object does not exist */
#define USFS_QUERY_PRESENT 1    /* queried object exists */

/* Sentinel module id returned when the extension is not loaded or the query
 * failed. Kept distinct from any valid id, which is always non-zero. */
#define USFS_MODULE_ID_NONE ((mid_t)0)

/* ------------------------------------------------------------------------- *
 * Tracing
 *
 * The logging macro formats a single line and appends it to USFS_LOG_PATH. In
 * non-DEBUG builds every trace macro expands to a no-op.
 * ------------------------------------------------------------------------- */

void usfs_log (const char * str);

#ifdef DEBUG
    #define USFS_LOGF(fmt, ...)                                                                    \
        do                                                                                         \
        {                                                                                          \
            char _usfs_line[USFS_LOG_BUFFER_SIZE];                                                 \
            snprintf (_usfs_line, sizeof (_usfs_line), "[%s] " fmt "\n", __func__, ##__VA_ARGS__); \
            usfs_log (_usfs_line);                                                                 \
        }                                                                                          \
        while (0)
#else
    #define USFS_LOGF(fmt, ...) \
        do                      \
        {                       \
        }                       \
        while (0)
#endif

/* ------------------------------------------------------------------------- *
 * Kernel extension lifecycle
 * ------------------------------------------------------------------------- */

/*
 * Returns the module id of the loaded USFS kernel extension, or
 * USFS_MODULE_ID_NONE when it is not loaded or the query fails.
 */
mid_t usfs_kext_get_module_id (void);

/*
 * Reports whether the kernel extension is currently loaded.
 * Returns one of USFS_QUERY_PRESENT / USFS_QUERY_ABSENT / USFS_QUERY_ERROR.
 */
int usfs_kext_is_loaded (void);

/* Loads the kernel extension from USFS_KEXT_PATH. Returns 0 on success. */
int usfs_kext_load (void);

/* Unloads the currently loaded kernel extension. Returns 0 on success. */
int usfs_kext_unload (void);

/* ------------------------------------------------------------------------- *
 * File system type registration (/etc/vfs)
 *
 * Publishes the mapping from the "usfs" type name to the type number the
 * kernel extension registered, so that mount, umount and df recognise mounted
 * USFS file systems. See the implementation for the entry format and for why
 * the number has to be read back from the device rather than hardcoded.
 * ------------------------------------------------------------------------- */

/*
 * Reports whether the USFS vfs type is registered in /etc/vfs.
 * Returns one of USFS_QUERY_PRESENT / USFS_QUERY_ABSENT / USFS_QUERY_ERROR.
 */
int usfs_type_is_registered (void);

/*
 * Adds the /etc/vfs entry for the USFS vfs type. The type number is queried
 * from the already configured device <logical_device_name>. Returns 0 on
 * success.
 */
int usfs_type_register (const char * logical_device_name);

/* Removes the USFS entry from /etc/vfs. Returns 0 on success. */
int usfs_type_unregister (void);

/*
 * Serializes all USFS configuration methods with the standard ODM
 * configuration lock. The returned token must be released exactly once.
 */
int usfs_config_lock_acquire (void);
int usfs_config_lock_release (int lock_id);

/* ------------------------------------------------------------------------- *
 * High-level device orchestration
 *
 * These implement the full Configure / Unconfigure flows shared by the tools.
 * Each is idempotent: it inspects current state and only performs the steps
 * that are still required.
 * ------------------------------------------------------------------------- */

/*
 * Brings device <logical_device_name> fully into service: loads and
 * configures the kernel extension if needed and registers the vfs type.
 * Returns 0 on success.
 */
int usfs_configure (const char * logical_device_name);

/*
 * Tears device <logical_device_name> fully out of service: unconfigures and
 * unloads the kernel extension if loaded and unregisters the vfs type.
 * Returns 0 on success.
 */
int usfs_unconfigure (const char * logical_device_name);

/* ------------------------------------------------------------------------- *
 * Argument parsing
 * ------------------------------------------------------------------------- */

/*
 * Parses the common "-l <logical_device_name>" invocation shared by the
 * Configure and Unconfigure methods. On success stores a pointer into argv
 * and returns 0; on malformed arguments logs the reason and returns non-zero.
 */
int usfs_parse_device_arg (int argc, char ** argv, const char ** logical_device_name);

#endif /* USFS_COMMON_H */

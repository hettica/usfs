/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Focused callback tests included after the existing client/syscall fixture.
 */

#define HIDDEN_FIXTURE_ENTRIES 32
#define HIDDEN_FIXTURE_INODE   400
#define HIDDEN_FIXTURE_HANDLE  77

static struct
{
    char paths[HIDDEN_FIXTURE_ENTRIES][USFS_PATH_MAX]; // Backend namespace independent of the client table.
    unsigned rename_calls;                             // Backend rename invocation count.
    unsigned release_calls;                            // Backend release invocation count.
    unsigned delete_calls;                             // Backend deletion invocation count.
    unsigned sequence;                                 // Ordering counter for release and delete.
    unsigned release_sequence;                         // Most recent backend release position.
    unsigned delete_sequence;                          // Most recent backend delete position.
    unsigned collisions;                               // Existing backing names absent from the client index.
    int hide_error;                                    // Injected hidden rename failure.
    int source_error;                                  // Injected source rename failure.
    int rollback_error;                                // Injected destination restoration failure.
    int delete_error;                                  // Injected hidden deletion failure.
    int zero_handle;                                   // Whether open deliberately returns fh zero.
    int initial_configuration_seen;                    // Whether init sees command-line settings.
    int expect_null_path;                              // Expected path contract for the callback matrix.
    unsigned callback_errors;                          // Unexpected callback paths or backend handles.
    unsigned handle_callbacks;                         // Handle callbacks observed by the matrix.
} hidden_fixture;

static int check_hidden_handle_callback (const char * path, const struct usfs_open_file * file_info)
{
    hidden_fixture.handle_callbacks++;

    if ((path == NULL) != hidden_fixture.expect_null_path || file_info == NULL || file_info->value != 0)
        hidden_fixture.callback_errors++;

    return 0;
}

static int hidden_path_index (const char * path)
{
    if (path == NULL)
        return -1;

    for (unsigned index = 0; index < HIDDEN_FIXTURE_ENTRIES; index++)
        if (hidden_fixture.paths[index][0] != '\0' && strcmp (hidden_fixture.paths[index], path) == 0)
            return (int)index;

    return -1;
}

static void add_hidden_fixture_path (const char * path)
{
    for (unsigned index = 0; index < HIDDEN_FIXTURE_ENTRIES; index++)
    {
        if (hidden_fixture.paths[index][0] != '\0')
            continue;

        strcpy (hidden_fixture.paths[index], path);
        return;
    }

    abort ();
}

static int hidden_getattr (
    const struct usfs_client_request * callback_request,
    const char * path,
    struct stat * metadata,
    struct usfs_open_file * file_info
)
{
    (void)callback_request;
    int index = hidden_path_index (path);

    if (file_info != NULL && hidden_fixture.expect_null_path)
    {
        check_hidden_handle_callback (path, file_info);
        index = 0;
    }

    if (index < 0 && path != NULL && strstr (path, "/.fuse_hidden") != NULL && hidden_fixture.collisions != 0)
    {
        hidden_fixture.collisions--;
        index = 0;
    }

    if (index < 0)
        return -ENOENT;

    memset (metadata, 0, sizeof (*metadata));
    metadata->st_mode = S_IFREG | 0600;
    metadata->st_ino = HIDDEN_FIXTURE_INODE + index;
    metadata->st_dev = 1;
    metadata->st_nlink = 1;
    metadata->st_blksize = CLIENT_DEFAULT_BLOCK_SIZE;

    return 0;
}

static int hidden_open (const struct usfs_client_request * callback_request, const char * path, struct usfs_open_file * file_info)
{
    (void)callback_request;
    if (hidden_path_index (path) < 0)
        return -ENOENT;

    file_info->value = hidden_fixture.zero_handle ? 0 : HIDDEN_FIXTURE_HANDLE;

    return 0;
}

static int hidden_rename (const struct usfs_client_request * callback_request, const char * source, const char * destination)
{
    (void)callback_request;

    hidden_fixture.rename_calls++;
    if (strstr (destination, "/.fuse_hidden") != NULL && hidden_fixture.hide_error != 0)
        return -hidden_fixture.hide_error;

    if (strcmp (source, "/source") == 0 && hidden_fixture.source_error != 0)
        return -hidden_fixture.source_error;

    if (strstr (source, "/.fuse_hidden") != NULL && hidden_fixture.rollback_error != 0)
        return -hidden_fixture.rollback_error;

    const int source_index = hidden_path_index (source);
    if (source_index < 0)
        return -ENOENT;

    const int destination_index = hidden_path_index (destination);

    if (destination_index >= 0)
        hidden_fixture.paths[destination_index][0] = '\0';

    strcpy (hidden_fixture.paths[source_index], destination);

    pthread_mutex_lock (&ns_test_lock);
    if (ns_test.active)
    {
        ns_test.rename_entered = 1;
        pthread_cond_broadcast (&ns_test_changed);
        ns_wait_for (&ns_test.release_rename, 1);
    }
    pthread_mutex_unlock (&ns_test_lock);

    return 0;
}

static int hidden_unlink (const struct usfs_client_request * callback_request, const char * path)
{
    (void)callback_request;
    hidden_fixture.delete_calls++;
    hidden_fixture.delete_sequence = ++hidden_fixture.sequence;

    if (hidden_fixture.delete_error != 0)
        return -hidden_fixture.delete_error;

    const int index = hidden_path_index (path);
    if (index < 0)
        return -ENOENT;

    hidden_fixture.paths[index][0] = '\0';

    return 0;
}

static int hidden_release (const struct usfs_client_request * callback_request, const char * path, struct usfs_open_file * file_info)
{
    (void)callback_request;
    if (hidden_fixture.expect_null_path)
        check_hidden_handle_callback (path, file_info);
    hidden_fixture.release_calls++;
    hidden_fixture.release_sequence = ++hidden_fixture.sequence;

    pthread_mutex_lock (&ns_test_lock);
    if (ns_test.active && ns_test.block_read)
    {
        ns_test.reads++;
        pthread_cond_broadcast (&ns_test_changed);
        ns_wait_for (&ns_test.release_read, 1);
    }
    pthread_mutex_unlock (&ns_test_lock);

    return hidden_fixture.expect_null_path || hidden_path_index (path) >= 0 ? 0 : -ENOENT;
}

static ssize_t hidden_read (
    const struct usfs_client_request * callback_request,
    const char * path,
    char * buffer,
    size_t size,
    off_t offset,
    struct usfs_open_file * file_info
)
{
    (void)callback_request;
    (void)offset;
    if (hidden_fixture.expect_null_path)
    {
        check_hidden_handle_callback (path, file_info);
        return 0;
    }
    if (hidden_path_index (path) < 0)
        return -ENOENT;

    if (size == 0)
        return 0;

    buffer[0] = 'x';

    return 1;
}

static int hidden_override_init (const struct usfs_client_request * request, const struct usfs_limits * limits, struct usfs_behavior * behavior)
{
    (void)request;
    (void)limits;
    hidden_fixture.initial_configuration_seen =
        behavior->remove_policy == USFS_REMOVE_IMMEDIATE && behavior->handle_paths == USFS_PATH_OMIT_FOR_HANDLE;
    behavior->remove_policy = USFS_REMOVE_DEFERRED;
    behavior->handle_paths = USFS_PATH_DEFAULT;
    return 0;
}

static struct usfs_client * new_hidden_fixture (const int zero_handle)
{
    memset (&hidden_fixture, 0, sizeof (hidden_fixture));
    hidden_fixture.zero_handle = zero_handle;
    add_hidden_fixture_path ("/file");

    const struct usfs_operations operations = { .getattr = hidden_getattr,
                                                .open = hidden_open,
                                                .read = hidden_read,
                                                .release = hidden_release,
                                                .rename = hidden_rename,
                                                .unlink = hidden_unlink };
    struct usfs_client * client = new_test_client (&operations, sizeof (operations), NULL);

    if (client == NULL)
        abort ();

    client->fd = 73;
    struct stat metadata = { 0 };

    hidden_getattr (get_callback_request (), "/file", &metadata, NULL);
    struct client_node * node = find_or_create_node (client, USFS_ROOT_ID, "file", &metadata);

    if (node == NULL)
        abort ();

    node->lookup_refs = 1;
    struct usfs_in_hdr request = { 0 };
    struct request_context context = { client, &request };
    const struct usfs_open_in opened = { FREAD | FWRITE, 0 };

    request.nodeid = node->id;
    handle_open_request (&context, (const char *)&opened, sizeof (opened));

    if (captured_reply.error != 0)
        abort ();

    return client;
}

static int unlink_hidden_fixture (struct usfs_client * client)
{
    struct usfs_in_hdr request = { 0 };
    struct request_context context = { client, &request };

    request.nodeid = USFS_ROOT_ID;
    handle_unlink_request (&context, "file", sizeof ("file"));

    return captured_reply.error;
}

static void release_hidden_fixture (struct usfs_client * client, const uint64_t nodeid)
{
    struct usfs_in_hdr request = { 0 };
    const struct usfs_release_in release = { client->handles->wire_fh, FREAD | FWRITE, client->handles->isdir };
    char message[sizeof (request) + sizeof (release)];

    request.version = USFS_PROTOCOL_VERSION;
    request.nodeid = nodeid;
    request.opcode = USFS_OP_RELEASE;
    request.len = sizeof (message);
    memcpy (message, &request, sizeof (request));
    memcpy (message + sizeof (request), &release, sizeof (release));
    handle_request (client, message, sizeof (message));
}

static void test_hidden_configuration (struct tap_state * tap)
{
    const struct usfs_operations operations = { .initialize = hidden_override_init };
    struct usfs_client_options options = { 0 };
    struct usfs_client * client = NULL;
    options.behavior.remove_policy = USFS_REMOVE_IMMEDIATE;
    options.behavior.handle_paths = USFS_PATH_OMIT_FOR_HANDLE;
    const int rc = usfs_client_create (USFS_CLIENT_API_VERSION, &operations, sizeof (operations), &options, NULL, &client);
    tap_ok (
        tap,
        rc == 0 && client != NULL && client->behavior.remove_policy == USFS_REMOVE_IMMEDIATE &&
            client->behavior.handle_paths == USFS_PATH_OMIT_FOR_HANDLE,
        "native options initialize both lifetime policies"
    );
    initialize_filesystem_once (client);
    tap_ok (
        tap,
        hidden_fixture.initial_configuration_seen && client->behavior.remove_policy == USFS_REMOVE_DEFERRED &&
            client->behavior.handle_paths == USFS_PATH_DEFAULT,
        "initialization sees and can override both lifetime policies"
    );
    (void)usfs_client_destroy (&client);
}

static void test_hidden_lifetime (struct tap_state * tap)
{
    struct usfs_client * client = new_hidden_fixture (true);
    struct client_node * node = find_child_node (client, USFS_ROOT_ID, "file");
    const uint64_t nodeid = node->id;

    tap_ok (tap, !client->behavior.remove_policy && !client->behavior.handle_paths, "hidden-file compatibility is enabled by default");
    tap_ok (
        tap,
        unlink_hidden_fixture (client) == 0 && find_child_node (client, USFS_ROOT_ID, "file") == NULL && node->hidden_names == 1,
        "unlink hides an open object even when its backend fh is zero"
    );

    struct resolved_handle_path resolved = { 0 };

    tap_ok (
        tap,
        resolve_node_handle_path (client, nodeid, 0, &resolved) == 0 && resolved.path != NULL &&
            strstr (resolved.path, "/.fuse_hidden") == resolved.path,
        "default callbacks resolve the managed hidden path"
    );
    struct stat metadata = { 0 };
    struct usfs_attr attributes = { 0 };

    hidden_getattr (get_callback_request (), resolved.path, &metadata, NULL);
    convert_node_attributes (client, &metadata, nodeid, &attributes);
    tap_ok (tap, attributes.nlink == 0 && node->backend_links == 1, "physical hidden links are excluded from logical metadata");
    struct fid_identity identity = { 0 };

    tap_ok (
        tap,
        copy_cached_fid_identity (client, nodeid, &identity) == ESTALE,
        "a managed hidden link cannot revive a deleted object's file identifier"
    );
    client->behavior.handle_paths = 1;
    omit_handle_callback_path (client, true, &resolved);
    tap_ok (tap, resolved.path == NULL, "nullpath_ok suppresses handle callback paths independently of backend fh");
    client->behavior.handle_paths = 0;
    release_hidden_fixture (client, nodeid);
    tap_ok (
        tap,
        captured_reply.error == 0 && hidden_fixture.release_calls == 1 && hidden_fixture.delete_calls == 1 &&
            hidden_fixture.release_sequence < hidden_fixture.delete_sequence && node->hidden_names == 0,
        "final RELEASE calls the backend before deleting the owned hidden entry exactly once"
    );
    (last_native_result = usfs_client_destroy (&client));
}

static void test_hard_remove_paths (struct tap_state * tap)
{
    struct usfs_client * client = new_hidden_fixture (false);
    struct client_node * node = find_child_node (client, USFS_ROOT_ID, "file");
    struct resolved_handle_path resolved = { 0 };
    client->behavior.remove_policy = 1;
    tap_ok (
        tap,
        unlink_hidden_fixture (client) == 0 && node->hidden_names == 0 && node->detached && hidden_fixture.rename_calls == 0,
        "hard_remove deletes the backend name immediately without hiding"
    );
    tap_ok (
        tap,
        resolve_node_handle_path (client, node->id, HIDDEN_FIXTURE_HANDLE, &resolved) == 0 && resolved.path == NULL,
        "hard_remove preserves detached handle callbacks without a former pathname"
    );
    release_hidden_fixture (client, node->id);
    (last_native_result = usfs_client_destroy (&client));
}

static ssize_t null_handle_write (
    const struct usfs_client_request * callback_request,
    const char * path,
    const char * data,
    size_t size,
    off_t offset,
    struct usfs_open_file * file_info
)
{
    (void)callback_request;
    (void)data;
    (void)offset;
    check_hidden_handle_callback (path, file_info);

    return (int)size;
}

static int null_handle_sync (const struct usfs_client_request * callback_request, const char * path, int datasync, struct usfs_open_file * file_info)
{
    (void)callback_request;
    (void)datasync;

    return check_hidden_handle_callback (path, file_info);
}

static int null_handle_flush (const struct usfs_client_request * callback_request, const char * path, struct usfs_open_file * file_info)
{
    (void)callback_request;
    return check_hidden_handle_callback (path, file_info);
}

static int null_handle_chmod (const struct usfs_client_request * callback_request, const char * path, mode_t mode, struct usfs_open_file * file_info)
{
    (void)callback_request;
    (void)mode;

    return check_hidden_handle_callback (path, file_info);
}

static int null_handle_chown (
    const struct usfs_client_request * callback_request,
    const char * path,
    uid_t uid,
    gid_t gid,
    struct usfs_open_file * file_info
)
{
    (void)callback_request;
    (void)uid;
    (void)gid;

    return check_hidden_handle_callback (path, file_info);
}

static int null_handle_truncate (
    const struct usfs_client_request * callback_request,
    const char * path,
    off_t size,
    struct usfs_open_file * file_info
)
{
    (void)callback_request;
    (void)size;

    return check_hidden_handle_callback (path, file_info);
}

static int null_handle_utimens (
    const struct usfs_client_request * callback_request,
    const char * path,
    const struct timespec times[2],
    struct usfs_open_file * file_info
)
{
    (void)callback_request;
    (void)times;

    return check_hidden_handle_callback (path, file_info);
}

static void exercise_null_handle_callbacks (struct usfs_client * client, const uint64_t nodeid)
{
    struct usfs_in_hdr request = { .nodeid = nodeid };
    const struct request_context context = { client, &request };
    const uint64_t handle = client->handles->wire_fh;
    const struct usfs_getattr_in attributes = { handle };
    const struct usfs_read_in read = { .fh = handle, .size = 1 };
    const struct usfs_write_in write = { .fh = handle };
    const struct usfs_flush_in flush = { .fh = handle, .flags = FREAD | FWRITE };
    struct usfs_fsync_in sync = { .fh = handle };
    const struct usfs_setattr_in changes = { .fh = handle,
                                             .valid = USFS_SET_MODE | USFS_SET_UID | USFS_SET_GID | USFS_SET_SIZE | USFS_SET_ATIME | USFS_SET_MTIME };

    client->ops.write = null_handle_write;
    client->ops.flush = null_handle_flush;
    client->ops.fsync = null_handle_sync;
    client->ops.fsyncdir = null_handle_sync;
    client->ops.chmod = null_handle_chmod;
    client->ops.chown = null_handle_chown;
    client->ops.truncate = null_handle_truncate;
    client->ops.utimens = null_handle_utimens;

    handle_getattr_request (&context, (const char *)&attributes, sizeof (attributes));
    handle_read_request (&context, (const char *)&read, sizeof (read));
    handle_write_request (&context, (const char *)&write, sizeof (write));
    handle_flush_request (&context, (const char *)&flush, sizeof (flush));
    handle_fsync_request (&context, (const char *)&sync, sizeof (sync));
    sync.flags = USFS_FSYNC_DIRECTORY;
    handle_fsync_request (&context, (const char *)&sync, sizeof (sync));
    handle_setattr_request (&context, (const char *)&changes, sizeof (changes));
    release_hidden_fixture (client, nodeid);
}

static void test_null_handle_paths (struct tap_state * tap)
{
    enum
    {
        NAMED_HANDLE,
        DETACHED_HANDLE,
        DIRECTORY_HANDLE,
        HARD_REMOVE_DEFAULT_PATH,
        HANDLE_PATH_CASES
    };

    for (unsigned path_case = 0; path_case < HANDLE_PATH_CASES; path_case++)
    {
        struct usfs_client * client = new_hidden_fixture (true);
        struct client_node * node = find_child_node (client, USFS_ROOT_ID, "file");

        client->behavior.handle_paths = path_case != HARD_REMOVE_DEFAULT_PATH;
        if (path_case == DETACHED_HANDLE || path_case == HARD_REMOVE_DEFAULT_PATH)
        {
            client->behavior.remove_policy = 1;
            unlink_hidden_fixture (client);
        }

        if (path_case == DIRECTORY_HANDLE)
        {
            client->handles->isdir = true;
            client->ops.releasedir = hidden_release;
        }

        hidden_fixture.expect_null_path = true;
        exercise_null_handle_callbacks (client, node->id);
        tap_ok (
            tap,
            hidden_fixture.callback_errors == 0 && hidden_fixture.handle_callbacks == 11 && captured_reply.error == 0,
            path_case == DETACHED_HANDLE    ? "detached zero backend handles receive NULL paths and valid file-info in every supported callback"
            : path_case == DIRECTORY_HANDLE ? "directory zero backend handles receive NULL paths in fsyncdir and releasedir"
            : path_case == HARD_REMOVE_DEFAULT_PATH ? "hard_remove preserves valid zero-handle callbacks even with nullpath_ok disabled"
                                                    : "named zero backend handles receive NULL paths and valid file-info in every supported callback"
        );
        (last_native_result = usfs_client_destroy (&client));
    }
}

static int check_named_path_attributes (
    const struct usfs_client_request * callback_request,
    const char * path,
    struct stat * metadata,
    struct usfs_open_file * file_info
)
{
    (void)callback_request;
    if (file_info != NULL || path == NULL || strcmp (path, "/file") != 0)
        hidden_fixture.callback_errors++;

    return hidden_getattr (get_callback_request (), path, metadata, file_info);
}

static void test_nullpath_pathname_operation (struct tap_state * tap)
{
    struct usfs_client * client = new_hidden_fixture (true);
    const struct client_node * node = find_child_node (client, USFS_ROOT_ID, "file");
    struct usfs_in_hdr request = { .nodeid = node->id };
    const struct request_context context = { client, &request };
    const struct usfs_getattr_in attributes = { 0 };

    client->behavior.handle_paths = true;
    client->ops.getattr = check_named_path_attributes;
    handle_getattr_request (&context, (const char *)&attributes, sizeof (attributes));
    tap_ok (
        tap,
        captured_reply.error == 0 && hidden_fixture.callback_errors == 0,
        "nullpath_ok retains the pathname and NULL file-info for ordinary metadata requests"
    );
    (last_native_result = usfs_client_destroy (&client));
}

static void test_hidden_failures (struct tap_state * tap)
{
    struct usfs_client * client = new_hidden_fixture (false);
    struct client_node * node = find_child_node (client, USFS_ROOT_ID, "file");

    hidden_fixture.collisions = CLIENT_HIDDEN_NAME_ATTEMPTS;
    tap_ok (
        tap,
        unlink_hidden_fixture (client) == EBUSY && hidden_fixture.rename_calls == 0 && !node->detached,
        "ten backing-name collisions fail without overwriting a file"
    );
    hidden_fixture.collisions = 2;
    tap_ok (
        tap,
        unlink_hidden_fixture (client) == 0 && hidden_fixture.rename_calls == 1,
        "hidden-name selection skips backing collisions absent from client indexes"
    );
    hidden_fixture.delete_error = EACCES;
    release_hidden_fixture (client, node->id);
    tap_ok (
        tap,
        captured_reply.error == 0 && node->hidden_names == 1 && hidden_fixture.delete_calls == 1 && node_has_references (node),
        "cleanup failure retains ownership while RELEASE succeeds"
    );
    hidden_fixture.delete_error = 0;
    (last_native_result = usfs_client_destroy (&client));
    tap_ok (
        tap,
        last_native_result == 0 && client == NULL && hidden_fixture.delete_calls == 2,
        "shutdown retries pending hidden cleanup exactly once"
    );

    client = new_hidden_fixture (false);
    node = find_child_node (client, USFS_ROOT_ID, "file");
    unlink_hidden_fixture (client);
    hidden_fixture.delete_error = EACCES;
    release_hidden_fixture (client, node->id);
    (last_native_result = usfs_client_destroy (&client));
    tap_ok (tap, last_native_result == -EACCES && hidden_fixture.delete_calls == 2, "unresolved shutdown deletion is returned by native destruction");

    client = new_hidden_fixture (false);
    hidden_fixture.hide_error = EACCES;
    tap_ok (
        tap,
        unlink_hidden_fixture (client) == EACCES && hidden_path_index ("/file") >= 0,
        "failed hidden rename leaves the original backend entry intact"
    );
    client->ops.rename = NULL;
    tap_ok (
        tap,
        unlink_hidden_fixture (client) == ENOSYS && hidden_path_index ("/file") >= 0,
        "missing rename support cannot silently destroy an open pathname object"
    );
    (last_native_result = usfs_client_destroy (&client));

    client = new_hidden_fixture (false);
    fail_mount_strdup = 1;
    tap_ok (
        tap,
        unlink_hidden_fixture (client) == ENOMEM && hidden_fixture.rename_calls == 0,
        "allocation failure precedes any hidden backend mutation"
    );
    (last_native_result = usfs_client_destroy (&client));
}

static void test_hidden_preparation_failures (struct tap_state * tap)
{
    struct usfs_client * client = new_hidden_fixture (false);
    struct client_node * node = find_child_node (client, USFS_ROOT_ID, "file");
    struct hidden_entry_operation preparation = { 0 };
    char * prepared_name = NULL;

    fail_mount_strdup = 1;
    tap_ok (
        tap,
        select_hidden_name (client, node->aliases, &preparation, &prepared_name) == -ENOMEM && hidden_fixture.rename_calls == 0,
        "hidden-name allocation failure cannot publish or modify a backend entry"
    );
    (last_native_result = usfs_client_destroy (&client));

    client = new_hidden_fixture (false);
    node = find_child_node (client, USFS_ROOT_ID, "file");
    char indexed_name[CLIENT_HIDDEN_NAME_CAPACITY];
    struct stat indexed_metadata = { 0 };

    hidden_getattr (get_callback_request (), "/file", &indexed_metadata, NULL);
    indexed_metadata.st_ino++;
    snprintf (indexed_name, sizeof (indexed_name), ".fuse_hidden%016llx%016llx", (unsigned long long)node->id, 1ull);
    find_or_create_node (client, USFS_ROOT_ID, indexed_name, &indexed_metadata);
    tap_ok (
        tap,
        unlink_hidden_fixture (client) == 0 && client->hidden_generation == 2 && find_alias (client, USFS_ROOT_ID, indexed_name) != NULL,
        "hidden-name selection also skips names retained only in client indexes"
    );
    /* An external actor can remove our owned backend entry first. */
    hidden_fixture.paths[0][0] = '\0';
    release_hidden_fixture (client, node->id);
    tap_ok (tap, captured_reply.error == 0 && node->hidden_names == 0, "ENOENT during final-close deletion is already cleaned");
    (last_native_result = usfs_client_destroy (&client));

    client = new_hidden_fixture (false);
    client->ops.getattr = NULL;
    tap_ok (
        tap,
        unlink_hidden_fixture (client) == ENOSYS && hidden_fixture.rename_calls == 0,
        "missing collision-check callback leaves an open entry untouched"
    );
    client->ops.getattr = hidden_getattr;
    client->ops.unlink = NULL;
    tap_ok (
        tap,
        unlink_hidden_fixture (client) == EROFS && hidden_fixture.rename_calls == 0,
        "missing cleanup callback leaves an open entry untouched"
    );
    (last_native_result = usfs_client_destroy (&client));
}

struct hidden_dispatch_job
{
    struct usfs_client * client; // Session containing the test namespace.
    uint64_t nodeid;             // Retained object or parent identifier for the operation.
    uint16_t opcode;             // Wire operation selected for the controlled interleaving.
};

static void * dispatch_hidden_job (void * argument)
{
    const struct hidden_dispatch_job * job = argument;
    struct client_thread_context callback = { 0 };
    struct usfs_in_hdr request = { 0 };
    char message[sizeof (request) + sizeof (struct usfs_rename_in) + sizeof (struct usfs_read_in) + sizeof ("source\0file")];
    size_t length = sizeof (request);

    callback.writebuf = malloc (USFS_MSG_MAX);
    if (callback.writebuf == NULL || ensure_thread_context_key () != 0 || pthread_setspecific (client_context_key, &callback) != 0)
        abort ();

    request.nodeid = job->nodeid;
    request.opcode = job->opcode;
    request.version = USFS_PROTOCOL_VERSION;
    request.unique = job->opcode;

    if (job->opcode == USFS_OP_UNLINK)
    {
        memcpy (message + length, "file", sizeof ("file"));
        length += sizeof ("file");
    }
    else if (job->opcode == USFS_OP_RELEASE)
    {
        const struct usfs_release_in release = { HIDDEN_FIXTURE_HANDLE, FREAD | FWRITE, 0 };

        memcpy (message + length, &release, sizeof (release));
        length += sizeof (release);
    }
    else if (job->opcode == USFS_OP_READ)
    {
        const struct usfs_read_in read_request = { HIDDEN_FIXTURE_HANDLE, 0, 1, 0 };

        memcpy (message + length, &read_request, sizeof (read_request));
        length += sizeof (read_request);
    }
    else
    {
        const struct usfs_rename_in rename_request = { .newparent = USFS_ROOT_ID, .oldnamelen = sizeof ("source") };

        memcpy (message + length, &rename_request, sizeof (rename_request));
        length += sizeof (rename_request);
        memcpy (message + length, "source\0file", sizeof ("source\0file"));
        length += sizeof ("source\0file");
    }

    request.len = (uint32_t)length;
    memcpy (message, &request, sizeof (request));
    handle_request (job->client, message, length);
    pthread_setspecific (client_context_key, NULL);
    free (callback.writebuf);

    return NULL;
}

static void test_hidden_concurrency (struct tap_state * tap)
{
    enum
    {
        UNLINK_VERSUS_RELEASE,
        RENAME_VERSUS_READ,
        FINAL_RELEASES_AFTER_FORGET,
        HIDDEN_CONCURRENCY_CASES
    };

    for (unsigned scenario = 0; scenario < HIDDEN_CONCURRENCY_CASES; scenario++)
    {
        struct usfs_client * client = new_hidden_fixture (false);
        struct client_node * node = find_child_node (client, USFS_ROOT_ID, "file");
        const uint64_t nodeid = node->id;

        if (scenario == RENAME_VERSUS_READ)
        {
            add_hidden_fixture_path ("/source");
            struct stat metadata = { 0 };

            hidden_getattr (get_callback_request (), "/source", &metadata, NULL);
            if (find_or_create_node (client, USFS_ROOT_ID, "source", &metadata) == NULL)
                abort ();
        }

        if (scenario == FINAL_RELEASES_AFTER_FORGET)
        {
            struct usfs_in_hdr request = { .nodeid = nodeid };
            const struct request_context context = { client, &request };
            const struct usfs_open_in opened = { FREAD | FWRITE, 0 };

            handle_open_request (&context, (const char *)&opened, sizeof (opened));
            unlink_hidden_fixture (client);
            if (identity_forget (client, nodeid, 1, 8) != 0)
                abort ();
        }

        pthread_mutex_lock (&ns_test_lock);
        memset (&ns_test, 0, sizeof (ns_test));
        ns_test.active = 1;
        ns_test.block_read = scenario == FINAL_RELEASES_AFTER_FORGET;

        struct hidden_dispatch_job first = { client,
                                             scenario == FINAL_RELEASES_AFTER_FORGET ? nodeid : USFS_ROOT_ID,
                                             scenario == UNLINK_VERSUS_RELEASE ? USFS_OP_UNLINK
                                             : scenario == RENAME_VERSUS_READ  ? USFS_OP_RENAME
                                                                               : USFS_OP_RELEASE };
        struct hidden_dispatch_job second = { client, nodeid, scenario == RENAME_VERSUS_READ ? USFS_OP_READ : USFS_OP_RELEASE };
        pthread_t first_thread;
        pthread_t second_thread;

        if (pthread_create (&first_thread, NULL, dispatch_hidden_job, &first) != 0)
            abort ();

        ns_wait_for (scenario == FINAL_RELEASES_AFTER_FORGET ? &ns_test.reads : &ns_test.rename_entered, 1);
        if (pthread_create (&second_thread, NULL, dispatch_hidden_job, &second) != 0)
            abort ();

        ns_wait_for (scenario == RENAME_VERSUS_READ ? &ns_test.reader_events : &ns_test.writer_events, 1);
        ns_test.release_rename = 1;
        ns_test.release_read = 1;
        pthread_cond_broadcast (&ns_test_changed);
        pthread_mutex_unlock (&ns_test_lock);
        pthread_join (first_thread, NULL);
        pthread_join (second_thread, NULL);

        const struct client_node * retained = find_node_by_id (client, nodeid);
        const int references_balanced = scenario == FINAL_RELEASES_AFTER_FORGET
                                            ? retained == NULL
                                            : retained != NULL && retained->operation_refs == 0 && retained->lookup_refs == 1 &&
                                                  retained->open_refs == (scenario == RENAME_VERSUS_READ ? 1u : 0u);
        const int valid = references_balanced && ns_test.errors == 0 && hidden_fixture.delete_calls == (scenario == RENAME_VERSUS_READ ? 0u : 1u) &&
                          hidden_fixture.release_calls == (scenario == RENAME_VERSUS_READ            ? 0u
                                                           : scenario == FINAL_RELEASES_AFTER_FORGET ? 2u
                                                                                                     : 1u);

        pthread_mutex_lock (&ns_test_lock);
        ns_test.active = 0;
        pthread_mutex_unlock (&ns_test_lock);
        tap_ok (
            tap,
            valid,
            scenario == UNLINK_VERSUS_RELEASE ? "unlink publication excludes concurrent RELEASE until the hidden path is ready"
            : scenario == RENAME_VERSUS_READ  ? "rename replacement excludes destination I/O until hidden publication completes"
                                              : "concurrent final releases after FORGET delete once and release both handles"
        );
        (last_native_result = usfs_client_destroy (&client));
    }
}

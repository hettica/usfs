// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT


enum data_limits
{
    DATA_READ_BYTE = OVERHEAD_READ_BYTE,
    DATA_WRITE_BYTE = OVERHEAD_WRITE_BYTE,
    DATA_SECONDS = 1,
    DATA_MAX_BATCH_BYTES = 1024 * 1024
};

struct data_run
{
    const struct overhead_server * server; // Calibration mount and daemon.
    bool direct;                           // Call the identical backend callbacks in this process.
    bool writing;                          // Write a validated replacement pattern instead of reading.
    bool shared_file;                      // Read one common inode, or use independent inodes.
    size_t transfer_bytes;                 // One application pread/pwrite size.
    unsigned clients;                      // Synchronized independent application workers.
    pthread_mutex_t lock;                  // Protects the common start gate.
    pthread_cond_t gate;                   // Releases fully prepared workers.
    unsigned ready;                        // Threads waiting to start.
    bool started;                          // Published start flag.
    uint64_t deadline_ns;                  // Monotonic end of the sustained sample.
};

struct data_worker
{
    struct data_run * run;           // Borrowed measurement configuration.
    int descriptor;                  // Application file descriptor for a USFS run.
    struct usfs_open_file file_info; // Direct callback handle for a direct run.
    unsigned char * buffer;          // Prefaulted read destination or write source.
    uint64_t operations;             // Completed, checked application operations.
    int result;                      // Worker success or failure.
};

static ssize_t invoke_data_operation (struct data_worker * worker)
{
    const struct data_run * run = worker->run;
    if (!run->direct)
    {
        if (run->writing)
            return pwrite (worker->descriptor, worker->buffer, run->transfer_bytes, 0);
        return pread (worker->descriptor, worker->buffer, run->transfer_bytes, 0);
    }

    size_t completed = 0;
    while (completed < run->transfer_bytes)
    {
        const size_t remaining = run->transfer_bytes - completed;
        const size_t count = remaining < USFS_MAX_DATA ? remaining : USFS_MAX_DATA;
        const ssize_t transferred = run->writing
                                        ? measured_write (NULL, NULL, (char *)worker->buffer + completed, count, (off_t)completed, &worker->file_info)
                                        : measured_read (NULL, NULL, (char *)worker->buffer + completed, count, (off_t)completed, &worker->file_info);
        if (transferred != (ssize_t)count)
            return -1;
        completed += count;
    }
    return (ssize_t)completed;
}

static void * execute_data_worker (void * argument)
{
    struct data_worker * worker = argument;
    struct data_run * run = worker->run;
    pthread_mutex_lock (&run->lock);
    run->ready++;
    pthread_cond_broadcast (&run->gate);
    while (!run->started)
        pthread_cond_wait (&run->gate, &run->lock);
    pthread_mutex_unlock (&run->lock);

    unsigned batch_operations = DATA_MAX_BATCH_BYTES / run->transfer_bytes;
    if (batch_operations == 0)
        batch_operations = 1;
    if (batch_operations > OVERHEAD_CLOCK_BATCH)
        batch_operations = OVERHEAD_CLOCK_BATCH;
    do
    {
        for (unsigned batch = 0; batch < batch_operations; ++batch)
        {
            const ssize_t count = invoke_data_operation (worker);
            if (count != (ssize_t)run->transfer_bytes)
            {
                worker->result = -1;
                return NULL;
            }
            if (!run->writing)
            {
                if (worker->buffer[0] != DATA_READ_BYTE || worker->buffer[run->transfer_bytes - 1] != DATA_READ_BYTE)
                {
                    worker->result = -1;
                    return NULL;
                }
            }
            worker->operations++;
        }
    }
    while (!overhead_interrupted && monotonic_ns () < run->deadline_ns);
    return NULL;
}

static int prepare_data_worker (struct data_worker * worker, const unsigned index)
{
    struct data_run * run = worker->run;
    char name[OVERHEAD_NAME_CAPACITY];
    char path[OVERHEAD_PATH_CAPACITY];
    const unsigned file_index = run->shared_file ? 0 : index;
    snprintf (name, sizeof (name), "data-%u", file_index);
    if (construct_overhead_path (path, sizeof (path), run->direct ? "" : run->server->mountpoint, name) != 0)
        return -1;

    worker->buffer = malloc (OVERHEAD_FILE_BYTES);
    if (worker->buffer == NULL)
        return -1;
    memset (worker->buffer, DATA_READ_BYTE, OVERHEAD_FILE_BYTES);
    worker->file_info.open_flags = O_RDWR;
    if (run->direct)
    {
        const int rc = open_file (NULL, path, &worker->file_info);
        if (rc != 0)
            return rc;
        struct mem_inode * inode = find_operation_inode (NULL, path, &worker->file_info);
        if (inode == NULL)
            return -1;
        memset (inode->data, DATA_READ_BYTE, inode->size);
    }
    else
    {
        worker->descriptor = open (path, O_RDWR);
        if (worker->descriptor < 0)
            return -1;
        if (ftruncate (worker->descriptor, OVERHEAD_FILE_BYTES) != 0)
            return -1;
        if (pwrite (worker->descriptor, worker->buffer, OVERHEAD_FILE_BYTES, 0) != OVERHEAD_FILE_BYTES)
            return -1;
    }
    if (run->writing)
        memset (worker->buffer, DATA_WRITE_BYTE, OVERHEAD_FILE_BYTES);
    return 0;
}

static int validate_data_worker (struct data_worker * worker)
{
    struct data_run * run = worker->run;
    if (worker->operations == 0)
        return -1;
    /* Both timed and full validation read actual returned data. Verify writes
     * changed the requested range and preserved the rest of the original file. */
    memset (worker->buffer, 0, OVERHEAD_FILE_BYTES);
    ssize_t count;
    if (run->direct)
        count = calibration_read (NULL, NULL, (char *)worker->buffer, OVERHEAD_FILE_BYTES, 0, &worker->file_info);
    else
        count = pread (worker->descriptor, worker->buffer, OVERHEAD_FILE_BYTES, 0);
    if (count != OVERHEAD_FILE_BYTES)
        return -1;
    for (size_t index = 0; index < OVERHEAD_FILE_BYTES; ++index)
    {
        const unsigned char expected = run->writing && index < run->transfer_bytes ? DATA_WRITE_BYTE : DATA_READ_BYTE;
        if (worker->buffer[index] != expected)
        {
            fprintf (stderr, "Failed to validate calibration data at byte %lu\n", (unsigned long)index);
            return -1;
        }
    }
    return 0;
}

static int dispose_data_workers (struct data_worker * workers, const unsigned count)
{
    int rc = 0;
    for (unsigned index = 0; index < count; ++index)
    {
        struct data_worker * worker = &workers[index];
        if (worker->run->direct && worker->file_info.value != 0)
        {
            if (memfs_release (NULL, NULL, &worker->file_info) != 0)
                rc = -1;
        }
        else if (worker->descriptor >= 0)
        {
            if (close (worker->descriptor) != 0)
                rc = -1;
        }
        free (worker->buffer);
    }
    return rc;
}

static int measure_data_run (struct data_run * run, FILE * output, FILE * callbacks, const unsigned round)
{
    struct data_worker workers[OVERHEAD_CLIENTS] = { 0 };
    pthread_t threads[OVERHEAD_CLIENTS];
    for (unsigned index = 0; index < run->clients; ++index)
    {
        workers[index].run = run;
        workers[index].descriptor = -1;
    }
    int rc = 0;
    for (unsigned index = 0; index < run->clients; ++index)
    {
        rc = prepare_data_worker (&workers[index], index);
        if (rc != 0)
            break;
    }
    if (rc != 0)
    {
        (void)dispose_data_workers (workers, run->clients);
        return rc;
    }
    if (pthread_mutex_init (&run->lock, NULL) != 0 || pthread_cond_init (&run->gate, NULL) != 0)
        exit (EXIT_FAILURE);
    for (unsigned index = 0; index < run->clients; ++index)
        if (pthread_create (&threads[index], NULL, execute_data_worker, &workers[index]) != 0)
            exit (EXIT_FAILURE);

    pthread_mutex_lock (&run->lock);
    while (run->ready != run->clients)
        pthread_cond_wait (&run->gate, &run->lock);
    struct server_response daemon_before = { 0 }, daemon_after = { 0 };
    struct rusage client_before = { 0 }, client_after = { 0 };
    struct kext_state kernel_before = { 0 }, kernel_after = { 0 };
    if (request_server_snapshot (run->server, 'B', &daemon_before) != 0 || getrusage (RUSAGE_SELF, &client_before) != 0 ||
        capture_kernel_status (&kernel_before) != 0)
        exit (EXIT_FAILURE);
    reset_callback_metrics ();
    __atomic_store_n (&overhead_shared->enabled, 1, __ATOMIC_RELEASE);
    const uint64_t started_ns = monotonic_ns ();
    run->deadline_ns = started_ns + DATA_SECONDS * (uint64_t)OVERHEAD_NANOSECONDS;
    run->started = true;
    pthread_cond_broadcast (&run->gate);
    pthread_mutex_unlock (&run->lock);
    uint64_t operations = 0;
    for (unsigned index = 0; index < run->clients; ++index)
    {
        if (pthread_join (threads[index], NULL) != 0)
            exit (EXIT_FAILURE);
        if (workers[index].result != 0)
            rc = -1;
        operations += workers[index].operations;
    }
    const uint64_t elapsed_ns = monotonic_ns () - started_ns;
    __atomic_store_n (&overhead_shared->enabled, 0, __ATOMIC_RELEASE);
    if (getrusage (RUSAGE_SELF, &client_after) != 0 || request_server_snapshot (run->server, 'E', &daemon_after) != 0 ||
        capture_kernel_status (&kernel_after) != 0)
        rc = -1;
    const struct callback_metric total = total_callback_metrics ();
    for (unsigned index = 0; index < run->clients; ++index)
        if (validate_data_worker (&workers[index]) != 0)
            rc = -1;
    const int cleanup_result = dispose_data_workers (workers, run->clients);
    pthread_cond_destroy (&run->gate);
    pthread_mutex_destroy (&run->lock);
    if (rc != 0 || cleanup_result != 0 || overhead_interrupted)
        return -1;

    const char * operation = run->writing ? "write" : "read";
    const char * path = run->direct ? "direct" : "usfs";
    fprintf (
        output,
        "%s,%s,%lu,%u,%s,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        operation,
        path,
        (unsigned long)run->transfer_bytes,
        run->clients,
        run->shared_file ? "shared" : "separate",
        round,
        overhead_shared->delay_us,
        overhead_shared->instrumented,
        (unsigned long long)operations,
        (unsigned long long)(operations * run->transfer_bytes),
        (unsigned long long)elapsed_ns,
        (unsigned long long)(resource_cpu_ns (&client_after) - resource_cpu_ns (&client_before)),
        (unsigned long long)(resource_cpu_ns (&daemon_after.usage) - resource_cpu_ns (&daemon_before.usage)),
        (unsigned long long)total.wall_ns,
        (unsigned long long)total.cpu_ns,
        (unsigned long long)total.calls,
        (unsigned long long)(kernel_after.accepted_requests - kernel_before.accepted_requests)
    );
    char label[OVERHEAD_NAME_CAPACITY];
    snprintf (
        label,
        sizeof (label),
        "%s-%s-%lu-%u-%s",
        operation,
        path,
        (unsigned long)run->transfer_bytes,
        run->clients,
        run->shared_file ? "shared" : "separate"
    );
    write_callback_metrics (callbacks, label, round);
    fflush (output);
    return 0;
}

static int run_data_suite (const struct overhead_server * server, FILE * output, FILE * callbacks, const unsigned rounds, const bool smoke)
{
    if (fill_calibration_backend () != 0)
        return -1;
    const size_t sizes[] = { OVERHEAD_PAGE_BYTES, USFS_MAX_DATA, USFS_MAX_DATA + sizeof (uint64_t), OVERHEAD_FILE_BYTES };
    for (unsigned round = 1; round <= rounds; ++round)
    {
        for (unsigned writing = 0; writing <= 1; ++writing)
        {
            for (unsigned size_index = 0; size_index < (smoke ? 1 : sizeof (sizes) / sizeof (sizes[0])); ++size_index)
            {
                for (unsigned client_index = 0; client_index < (smoke ? 1 : 2); ++client_index)
                {
                    for (unsigned shared_file = 0; shared_file <= (writing == 0 && client_index != 0 ? 1u : 0u); ++shared_file)
                    {
                        for (unsigned variant = 0; variant <= 1; ++variant)
                        {
                            struct data_run run = { .server = server,
                                                    .direct = (variant + round) % 2 != 0,
                                                    .writing = writing != 0,
                                                    .shared_file = shared_file != 0,
                                                    .transfer_bytes = sizes[size_index],
                                                    .clients = client_index == 0 ? 1 : OVERHEAD_CLIENTS };
                            fprintf (
                                stderr,
                                "data round=%u operation=%s path=%s bytes=%lu clients=%u files=%s\n",
                                round,
                                run.writing ? "write" : "read",
                                run.direct ? "direct" : "usfs",
                                (unsigned long)run.transfer_bytes,
                                run.clients,
                                run.shared_file ? "shared" : "separate"
                            );
                            const int rc = measure_data_run (&run, output, callbacks, round);
                            if (rc != 0)
                                return rc;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

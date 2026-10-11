# Backend-neutral overhead calibration

Build with the normal out-of-source AIX workflow and run `make test`. The
benchmark is produced as `build/testing/bin/usfs_overhead`; it links the ordinary
production client library. Primary measurements use the ordinary release kernel.
Run only on an authorized, otherwise idle AIX host as root.

The controlled backend starts from the project's memory example, preallocates
four 1 MiB files, and replaces read/write service with bounded memory copies.
Timed read/write callbacks do no allocation, backing filesystem I/O, metadata
updates, or global backend locking. Direct and mounted paths invoke the same
callback adapters, with the direct path reproducing the 64 KiB callback splits.
This measures an achievable framework cost for this backend shape and host; it
does not establish a universal percentage for an arbitrary customer's backend.

```sh
mkdir -p /ws/overhead-mount
bash tests/performance/run-benchmarks.sh "$PWD" "$PWD/build" \
    /ws/overhead-results /ws/overhead-mount
bash tests/performance/measure-file-identifiers.sh "$PWD" "$PWD/build" \
    /ws/overhead-results /ws/overhead-mount
bash tests/performance/compare-user-privileges.sh "$PWD" "$PWD/build" \
    /ws/overhead-results /ws/overhead-mount
perl tests/performance/enforce-timeout.pl 600 build/testing/bin/usfs_overhead \
    /ws/overhead-mount /ws/overhead-results/data-pool1-delay1000 \
    1 1000 1 0 3 data
perl tests/performance/audit-measurements.pl /ws/overhead-results
perl tests/performance/summarize-report.pl /ws/overhead-results
perl tests/performance/generate-tables.pl /ws/overhead-results
```

Use a new output directory per campaign. Each raw output file is exclusively
created and cannot overwrite an earlier run. The runner rejects existing USFS
mounts and checks that the installed kernel matches the chosen release build.
The identifier runner temporarily installs the matching testing kernel and
restores the ordinary release kernel on exit. Identifier costs are composite
diagnostics and are separate from production performance estimates.
The final single-worker delayed run checks whether conclusions about worker
count for fast callbacks also apply to blocking callbacks. Run configurations
sequentially, with no other mounted USFS workload. Run `make test` and
`make test-e2e` again after the campaign and retain both logs.

For a bounded smoke test or individual configuration:

```sh
perl tests/performance/enforce-timeout.pl 120 build/testing/bin/usfs_overhead \
    /ws/overhead-mount /ws/unique-smoke-prefix 8 0 1 0 1 smoke
```

Arguments after the output prefix are worker pool size, injected callback delay
in microseconds, instrumentation flag, legacy-create fallback flag, rounds, and
suite (`metadata`, `data`, `smoke`, `data-smoke`, or `identifiers`). Both flags accept 0 or 1.
Identifiers require the matching testing kernel. The fallback mode exercises
the public `create` callback through current `CREATE_ATTR` protocol requests
when the `create_attr` callback is absent.

Set `USFS_OVERHEAD_UID` to a positive numeric UID for a data-only ordinary-user
campaign. Files are owned by that UID. The mounted daemon remains root, while
the application drops supplementary groups and both user/group identities.
Output files and one status descriptor are opened before the drop; no device
permissions or host accounts are changed. The identity runner uses UID 10000
and repeats a root reference with the same executable. This matters because
ordinary-user writes can require additional backend attribute requests for
privilege-bit handling, which privileged callers bypass.

Data samples run for at least one second, alternating direct/mounted order by
round. They use 4 KiB, 64 KiB, 64 KiB plus eight bytes, and 1 MiB application
transfers with one or four clients. Four-client reads include shared and separate
inodes; writes use separate inodes. Buffers and files are prepared before timing.
Every sample checks returned data and validates the complete resulting file.
The `read_bytes` CSV column is the application transfer size for either direction.

Metadata preparation and cleanup are outside each target operation's timer.
There are 200 individual samples per operation per round (two in smoke mode).
Both cached and first-fetch mapped reads are selected by retaining a shared
mapping; mapped flush includes `msync(MS_SYNC)` and an `fsync` completion barrier.
Missing-path and unsupported-operation probes require their documented or
observed error outcomes. Pathname and descriptor truncation samples require
successful operations.

Instrumentation brackets callbacks with a monotonic wall clock and AIX
`getrusage(RUSAGE_THREAD)` CPU snapshots. The bracket includes the injected delay
and timer/probe costs. Per-callback counters use shared atomic updates. Primary
uninstrumented measurements omit those probes; compare both modes to assess
observer effects. Lifecycle logs include control-pipe and worker start/stop
costs, and exclude initial memory-backend preparation.

For serial operations, elapsed minus measured callback wall time estimates the
remaining framework/application/AIX cost. Under concurrency, callback wall
durations overlap: **never subtract their sum from application elapsed time**.
Instead compare throughput and process CPU per operation, subtracting summed
callback thread CPU only in instrumented runs. Sustained time divided by total
operations under concurrency is reciprocal throughput, not request latency.

Retain raw CSV, logs, exact source/binary hashes, compiler flags, AIX level,
LPAR dispatch statistics, failed screening runs, and final mount/daemon health.
The raw-data audit checks sample durations, byte counts, request splitting,
ordinary-user attribute fan-out and callback accounting before interpretation.
It writes `data-exclusions.csv` when a sample contains an extra background
request. The summarizer excludes the complete direct/USFS pair, preserves all
raw samples, and requires at least two uncontaminated pairs per workload shape.
Instrumented exclusions identify `syncfs`; uninstrumented exclusions disclose
the extra global request without claiming its opcode. Retained pair counts
and nominal confidence intervals are recorded in `data-summary.csv`.
The fixtures contain no durable storage, so these tests measure synchronization
framework cost rather than storage durability latency. Cache-hit paths may
invoke no backend callbacks and must be presented separately from RPC paths.

## Files

| File | Responsibility |
|---|---|
| `benchmark.c` | Run the calibration backend and coordinate benchmark suites. |
| `callback_measurements.h` | Adapt backend callbacks and measure their service costs. |
| `data_transfers.h` | Measure and validate direct and mounted reads and writes. |
| `filesystem_operations.h` | Exercise native filesystem operations and dispatch suites. |
| `timing.h` | Collect clocks, CPU usage and callback counters for calibration. |
| `run-benchmarks.sh` | Run the production overhead estimation campaign. |
| `measure-file-identifiers.sh` | Measure test-only identifier and directory-sync diagnostics. |
| `compare-user-privileges.sh` | Compare root and ordinary-user read/write costs. |
| `enforce-timeout.pl` | Bound a benchmark process and clean up on timeout. |
| `audit-measurements.pl` | Validate raw samples and record contaminated-pair exclusions. |
| `summarize-report.pl` | Generate the full statistical report and summary CSV files. |
| `generate-tables.pl` | Format summary CSV files as Markdown report tables. |

Historical campaign logs and source hashes retain the names used when those
measurements were collected. The filenames above are the current entry points.

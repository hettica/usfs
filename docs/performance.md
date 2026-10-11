# Initial performance test

Tests ran on 2026-10-10 on IBM POWER with AIX 7.3, base runtime
(bos.rte) version 7.3.3.2. The server had one virtual CPU, 2 GiB of memory,
and shared CPU capacity with other systems.

The tests used a backend, the code that supplies file data to USFS, that kept
its data in memory. Comparing direct calls to that code with file access
through USFS measured the extra time USFS adds to each operation.

- For 4 KiB transfers, USFS added about 21 microseconds per read,
  22 microseconds per write as root, and 42 microseconds per write as an
  ordinary user. A microsecond is one millionth of a second. Ordinary-user
  writes need an extra request to check file attributes.
- USFS transfers data in chunks of up to 64 KiB. Reading or writing 1 MiB
  therefore needs 16 sequential requests, adding about 429 microseconds
  per read or 492 microseconds per write as root.
- Getting file information by its name required three requests to the backend;
  getting information about an already open file required one.
- With four applications accessing files at once, eight worker threads moved
  about four times as much data as one worker when each backend operation
  took about one millisecond. For fast operations on data in memory, one
  worker was faster.

These results describe the extra cost of using USFS on this test server.
The time a backend spends storing or retrieving data is additional. Actual
performance depends on the backend, hardware, and workload.

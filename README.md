# USFS: userspace file systems for IBM AIX

USFS is a 64-bit AIX filesystem kernel extension and a userspace client library. 
It provides FUSE-like functionality on AIX using an AIX-specific implementation.
A [FUSE adapter](https://github.com/hettica/aix-fuse) can make the client library compatible with the Linux FUSE API, making 
it easier to port projects that depend on FUSE.

## Current project state

Today it's a technical preview. The core functionality works, with known issues.
You can run the provided examples or conduct your own experiments.
Do not use this preview in production. Kernel extension failures can crash the system.
Upgrade from this version will not be supported.

## Build

Build natively on AIX with GCC, CMake 3.20+, GNU Make, Bash, RPM build tools,
and the native AIX development tools:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_C_COMPILER=gcc
cd build
make package
```

## Install and use

Run as root on an isolated, recoverable AIX host:

```sh
# Install and check status
rpm -ivh "build/packages/usfs-0.1.0-1.aix$(uname -v).$(uname -r).ppc.rpm"
usfsctl

### Run memfs example
# Main session
mkdir -p /mnt/usfs-mem
usfs_memfs /mnt/usfs-mem

# Separate session
printf 'USFS works\n' >/mnt/usfs-mem/hello.txt
cat /mnt/usfs-mem/hello.txt
umount /mnt/usfs-mem

### Run the read-only mirror example
# Main session
mkdir -p /mnt/usfs-mirror
usfs_mirror --source=/usr /mnt/usfs-mirror

# Separate session
ls /mnt/usfs-mirror
umount /mnt/usfs-mirror

```
The package installs `/usr/lib/libusfs.a`, `/usr/include/usfs/usfs.h`, and
`usfs.pc`. The public header documents requests, callbacks, options and
lifecycle contracts; the examples demonstrate their use.

In-place upgrades and replacement installs are rejected. To reinstall, unmount
all USFS filesystems, stop the daemons, and prevent new device opens. Then run
`rpm -e usfs` followed by `rpm -ivh` with the new RPM.

## Trace

USFS integrates with standard AIX tracing facilities to capture kernel and
request activity for diagnosis:

```sh
# Start a root-owned session and select where to save the raw trace.
usfsctl trace start --profile requests --output /tmp/usfs.trace
usfsctl trace status

# Reproduce the event. Pause and resume are optional.
usfsctl trace pause
usfsctl trace resume

# Stop records the raw trace and creates /tmp/usfs.trace.txt.
usfsctl trace stop

# Reformat the raw trace later, optionally choosing a report path.
usfsctl trace report /tmp/usfs.trace --output /tmp/usfs-report.txt
```

Possible profiles are `core`, `requests` (default), `full`.

## Known limitations

Concurrent live unload is unsafe. An open syscall racing with `CFG_TERM` and `SYS_KULOAD` causes a kernel fault.
Should not be reproducible during manual sequential execution of examples above.

## Roadmap

- Technical preview — current stage.
- Alpha release, first test and audit reports.
- Beta release with kernel instrumentation capabilities (fault injection, coverage reports).
- Clarified compatibility and system requirements, documented WPAR support, performance tests.
- First stable general-availability release.
- s3fs and sshfs on AIX.

Further development will depend on community demand. Feedback is highly appreciated.

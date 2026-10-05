# USFS: userspace file systems for IBM AIX

USFS is a 64-bit AIX filesystem kernel extension with a FUSE-compatible
userspace client library. It brings FUSE-like functionality to AIX through an
AIX-specific implementation. Compatibility is limited to the client library
interface, with the aim of making projects such as sshfs and s3fs easier to port.

## Current project state

Today it's a technical preview. The core functionality works, with known issues.
You can run the provided examples or conduct your own experiments. 
Do not use this preview in production. Kernel extension failures can crash the system.
Upgrade from this version will not be supported.

## Build

Build natively on AIX with GCC, CMake 3.20+, GNU Make, Bash, RPM build tools,
and the native AIX development tools:

```sh
cmake -S . -B out -G "Unix Makefiles" -DCMAKE_C_COMPILER=gcc
cmake --build out --target package
```

The unsigned RPM is written to `out/packages/`, with the build host's AIX
release in its filename. Installation requires the same `AIX-rpm` version
and release as the build host; compatibility with other system levels is not claimed.

## Install and use

Run as root on an isolated, recoverable AIX host:

```sh
# Install and check status
rpm -ivh "out/packages/usfs-0.1.0-1.aix$(uname -v).$(uname -r).ppc.rpm"
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

# Resident kernel proxy

USFS consists of `usfs_proxy.kext`, which normally remains loaded until reboot,
and `usfs.kext`, which implements the filesystem and can be replaced on a running
AIX system after its users have been torn down. Existing mounts are not migrated.

## Why the proxy exists

The previous MPX device registered callbacks inside the unloadable filesystem
extension. On real POWER/AIX, a pathname open could enter `ddmpx` before its first
USFS lifetime claim while another thread successfully removed the device switch
entry and unloaded the extension. Resuming the callback then executed freed code.
An implementation-side counter or a delay cannot protect that pre-entry interval.

Every supported device-switch callback now points into the resident proxy. Under
its short dispatch lock, the proxy checks admission, copies the callback table,
and acquires a reference. It drops the lock before calling the implementation and
releases the reference only after execution has returned to the resident code.
Consequently, an already admitted call prevents unloading, while a late call can
be rejected without touching implementation memory.

## Normal lifecycle

Configuration loads the proxy before the implementation. Its internal attachment
ABI has an explicit version, structure size, build identity, and typed callbacks.
Only kernel code can attach callbacks. The implementation imports proxy services;
the proxy has no static dependency on implementation symbols. Loader ownership,
memory pinning, and dispatch references are separate requirements.
An atomic setup gate serializes first-time proxy initialization, attachment, and
explicit maintenance removal before the dispatch lock exists.
An attached table remains unpublished until initialization activates it; the
proxy rejects concurrent teardown requests during that interval.

The proxy owns the device-switch registration and the associated major/minor
reservation. Ordinary unconfiguration removes `/dev/usfs0` but retains those
numbers while the resident registration exists. Reconfiguration reuses them.

The proxy also owns the AIX pager registration. It calls `vm_mounte` once when
the resident proxy initializes and keeps that registration across ordinary
implementation unload/reload. A pager request enters a resident trampoline,
acquires the same dispatch reference as a device call, and then invokes the
attached implementation's strategy callback. If the implementation is detached
or admission is closed, the trampoline completes each buffer with `EIO` without
entering implementation code. Normal implementation teardown still releases
its own filesystem and I/O resources, but does not call `vm_umount`.
Explicit unsafe proxy removal calls `vm_umount` before removing the proxy's
device registration. The proxy ABI is 3 because the callback table, pager
contract, and termination seal must agree across both modules. `usfsctl kexts`
shows `pager=1` while the resident registration is present.

This ownership change addresses a measured reload-cycle memory problem. On an
AIX 7.3 SMP guest, the former implementation-owned `vm_mounte`/`vm_umount`
pair with 64 pager buffers added 52 pinned 4 KiB-equivalent pages per stable
cycle. Halving the buffer count halved the increase. With the resident pager,
pinned pages rose during the first 20 cycles, then changed by only 18
over cycles 20 through 100. An AIX 7.2 guest changed by two pinned pages over
100 cycles. These tests establish that the repeatable growth is absent under
the tested conditions; they do not identify the internal AIX owner of the
previously retained pages. See the local memory-investigation evidence for
the measurements and limits.

Normal termination first enters a closing phase. New opens and channel allocations
are rejected, while close and channel deallocation can finish under dispatch
references. Before freeing implementation resources, the implementation asks the
proxy to seal admission; sealing fails busy if any callback remains. Mounts,
channels, connections, and other resources still prevent termination. Busy rejection
restores admission if the implementation remains active. A direct `CFG_TERM` sent
to the implementation module is rejected after attachment; the proxy uses a
private configuration callback for termination, lifecycle queries, and deferred
coverage export. Partial cleanup retains
the implementation and permits retry with ordinary dispatch closed.

Only after callbacks have returned and the implementation reports `DOWN` does
the proxy erase its callback table. Userspace can then request implementation
unloading. Deferred coverage export must finish before this final transition.
A replacement is initialized before forwarding reopens. Detached device calls
receive `ENODEV`; temporarily closed admission returns `EBUSY`.

There is no unload grace-period timer, delay, forced reference disposal, or
assumption that a successful loader request alone proves safe cleanup.

Use the usual AIX configuration commands:

```sh
usfsctl kexts
usfsctl status
# Stop backends and unmount USFS filesystems first.
rmdev -l usfs0
usfsctl kexts       # Proxy remains loaded; implementation is absent.
mkdev -l usfs0
```

`usfsctl kexts` inspects both extensions even without the status device. It reports
loaded identity, proxy ABI and variant, attachment state, references, and recorded
implementation lifecycle. Numeric proxy states are 0 (empty), 1 (active), 2
(closed), and 3 (closing). Query failures are not reported as absence.
Default/status output also reports filesystem health when the implementation
is available.
The running build fields are source-content identities for the loaded proxy and
implementation variants. A file replacement does not change either running ID.

AIX can keep an unloaded, zero-reference kernel object in its loader list after
`SYS_KULOAD` succeeds. Thus `genkex` can briefly list an old implementation
after `usfsctl kexts` reports it absent. A test can run the documented
`slibclean` command on an isolated guest, then require that no old `usfs.kext`
entry remains. A remaining entry after cleanup indicates a live loader
reference and fails the no-growth check. Normal configuration does not run
system-wide loader cleanup or wait for a guessed grace period. See IBM's
[kernel extension unload behavior](https://www.ibm.com/docs/en/aix/7.3.0?topic=extensions-unloading-kernel)
and [slibclean command](https://www.ibm.com/docs/ssw_aix_71/com.ibm.aix.cmds5/slibclean.htm).

Compatible package upgrades reuse the resident proxy. Updating a proxy file on
disk does not replace its loaded image. An incompatible resident ABI requires a
reboot or explicitly unsafe maintenance. Normal package removal leaves the inert
resident proxy and its device-number reservation until that boundary.

## Explicitly unsafe proxy removal

```sh
usfsctl proxy unload --unsafe
# Interactive confirmation: UNLOAD PROXY
```

This root-only command is different from normal implementation unloading. It
requires the implementation to be absent and refuses attached callbacks, active
references, and competing control operations. It removes registration before
unpinning and requests unloading, then verifies loader absence before releasing
device numbers. Failures preserve retryable ownership where possible.

**The command can still crash AIX.** AIX might already have dispatched a callback
that has not entered the proxy and therefore is not counted. Removing the proxy
reintroduces that pre-entry exposure. No delay is presented as a safety guarantee.
Reboot is the normal way to replace the resident proxy safely.

Unattended use additionally requires `--confirm-unsafe-unload`. Production package
operations never invoke this command automatically. The isolated test runner may
use it to change proxy variants after stopping workers, unconfiguring the
implementation, and exporting available coverage. Such maintenance is logged
separately from safe implementation-unload tests; a crash fails the run.

## Verification and coverage

Native unit tests exercise callback accounting, interface validation, overflow,
busy rollback, partial teardown, service failures, and maintenance preconditions.
Test-only barriers deterministically stop callbacks before admission, after
admission, and after implementation return, and pause termination during closing.
The pre-admission test unloads the implementation and resumes the callback;
the other schedules prove unloading is rejected until references are returned.
The closing test releases a real status channel before sealing. Barriers are
explicitly released.

`make test` runs the lifecycle regression with the existing integration suite;
`make test-e2e` first runs production mount, verified I/O, safe unload/reload,
and repeated concurrent status-device opens during reload cycles, then exercises
the broader filesystem journeys with the testing variant. The production stress
defaults to 12 cycles and four opener processes; set `USFS_STRESS_CYCLES` and
`USFS_STRESS_WORKERS` to change them for an isolated run.
Production builds contain neither barrier controls nor coverage instrumentation.

The coverage proxy has its own registry. `usfs_coverage proxy-snapshot DIRECTORY`
exports and resets its counters without opening an implementation device. The
coverage collector reports it as a separate component and merges it with normal
line, branch, and function coverage. Native unit results cover terminal paths
whose final counters cannot be exported after unloading. Coverage percentages are
informational; missing state-transition and failure evidence must be reviewed.

Local destructive tests use disposable Docker AIX VMs. Real POWER SMP
qualification uses a separate temporary AIX LPAR with two online virtual CPUs
and corresponding allocated capacity. Record guest topology and per-CPU activity
with results. Delete the temporary LPAR and its owned resources after a clean
run; retain them for investigation if any acceptance test fails.

# Pager callbacks for mapped files

USFS uses a separate backend resource for VMM page I/O. AIX does not supply a
reliable per-open handle or mapping identifier to page faults, so a backend
must not choose one of the file's ordinary open handles for a pager request.
The native API defines this resource as `struct usfs_pager_file`.

`pager_acquire` obtains one resource for the file object when its first VMM
segment is prepared. The callback receives the current path when one exists
and can inspect the mount-scoped object identity with
`usfs_request_object_identity()`. The backend must bind the resource to that
object, even if it is later renamed, replaced at its old path, or unlinked.
If it cannot retain the object safely, it must reject acquisition. The pager
resource is independent of every ordinary `open` handle; a zero resource value
is valid.

USFS calls `pager_authorize` for **each** mapping with that caller's credentials
and the requested read or write access. Before admitting a writable mapping or
a cached ordinary write, it calls `pager_enable_write` for the same resource.
Backends must enforce their own authorization policy in these callbacks. A
policy that requires a per-open token unavailable to AIX must reject mapping.

Page faults and writeback call `pager_read` and `pager_write` with the acquired
resource, an offset, and a byte buffer. These calls have no reliable caller
identity or pathname. They may overlap across processes and threads, so the
backend must synchronize access to the retained object. Ordinary uncached
`read` and `write` continue to use the exact handle returned by `open`.

The resource remains available after close, rename, unlink, and the final
`munmap` while the VMM segment or cached I/O still needs it. `pager_release`
ends the lease after the segment is deleted. A failed segment deletion retains
the vnode and pager lease rather than freeing storage the VMM might address.

A successful map increments the vnode mapping count and holds the vnode.
Unmap drops that count and hold, while the pager resource remains until VMM
teardown. AIX also supplies no usable per-open handle to vnode-wide fsync;
USFS sends that request with object identity instead of selecting another
open description.

A backend must supply the pager callbacks to support `mmap`; missing callbacks
make mapping fail with `ENOSYS`. The bundled memory filesystem and read-only
mirror demonstrate object-scoped implementations. See
[`usfs.h`](../src/client/include/usfs/usfs.h) for the callback signatures and
[`mmap_pager.sh`](../tests/integration/aix/contract/mmap_pager.sh) for the AIX
contract tests.

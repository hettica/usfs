# Conditional namespace mutations

Protocol 12 and client API 4 bind `unlink`, `rmdir`, and both sides of `rename` to the directory entries AIX selected.
The kernel sends the selected vnode IDs. The client checks those IDs against its namespace aliases before invoking a
callback. A stale selection returns `ESTALE` without a callback or alias change.

The callback receives a `usfs_expected_entry` for each affected name. It contains a borrowed path, the expected parent
identity, and either the expected child identity (`exists == 1`) or expected absence (`exists == 0`). The destination of
a rename can be absent. The identifiers refer to objects, not open handles, and must remain distinguishable while AIX
can retain their vnodes.

The backend must compare **both** the parent and child expectations and perform the mutation as one atomic namespace
operation. If either expectation fails, return `-ESTALE` without changing anything. This applies to a same-object rename
and to internal deferred-unlink moves, rollback, and hidden-name cleanup. An absent destination must still be absent
when the rename commits. Successful callbacks are the only point after which the client publishes alias changes.

For a backend with its own namespace, hold its namespace lock across comparison and mutation. The bundled memfs does
this with its stable, never-reused inode identities. A backend that delegates pathname operations to another filesystem
must be able to exclude every other writer to that backing namespace for the full comparison and mutation. Checking with
`stat` and later calling pathname `unlink` or `rename` while another writer can replace names does **not** satisfy the
contract. If the backend cannot guarantee the atomic comparison, leave the callback unsupported and return `-ENOSYS`.

The read-only mirror does not implement these callbacks. The pathname daemon under `tests/tools` is a test fixture for a
private backing tree and serializes its own namespace writes; it must not be used as a general-purpose mutable backend
over a tree that other processes can edit.

# Unsupported filesystem operations

This list concerns files and directories on a USFS mount. The `/dev/usfs0`
control device has separate operations. A backend cannot add an operation that
the USFS kernel extension does not implement.

The kernel entry points listed below are absent or return `ENOSYS`. AIX can
handle some application calls before reaching those entry points, or return a
different error from a wrapper. The observed results in the table were checked
on POWER AIX 7.3 with a USFS memory filesystem; other rows describe the
current kernel implementation.

| Application operation | Current result | USFS entry point |
| --- | --- | --- |
| `pathconf` and `fpathconf` | Unsupported. `_PC_NAME_MAX` queries returned `ENOSYS`. | `vnop_finfo` |
| `fclear` | Unsupported; returns `ENOSYS` in the integration test. | `vnop_fclear` |
| File or directory `ioctl` | Unsupported; the file probe returned `ENOSYS`. | `vnop_ioctl` |
| `mknod` and FIFO creation with `mkfifo` | Special-file creation is unsupported. `mkfifo` returned `EINVAL` in the probe. | `vnop_mknod` |
| Traditional and extended ACL reads, writes, and ACL-type control queries (`statacl`, `fstatacl`, `chacl`, `fchacl`, `aclx_get`, `aclx_put`, and their descriptor variants) | Unsupported; ordinary mode-bit operations such as `chmod` are separate. | `vnop_getacl`, `vnop_setacl`, `vnop_getxacl`, `vnop_setxacl`, `vfs_aclxcntl` |
| Privilege control list (PCL) reads and writes | Unsupported. | `vnop_getpcl`, `vnop_setpcl` |
| Extended-attribute operations (`getea`, `setea`, `listea`, `removeea`, `statea`, and their link and descriptor variants) | Unsupported. | `vnop_getea`, `vnop_setea`, `vnop_listea`, `vnop_removeea`, `vnop_statea` |
| Quota control through `quotactl` | Unsupported. | `vfs_quotactl` |
| Filesystem control through `fscntl`, including filesystem extension | Unsupported. | `vfs_cntl` |
| Revoking access to a file with `revoke` | Unsupported. | `vnop_revoke` |

The following AIX interfaces also have no USFS implementation:

- `vnop_memcntl`: file memory-attachment control (`F_ATTACH`, `F_DETACH`,
  `F_ATTINFO`). This is distinct from supported file mapping with `mmap`.
- `vnop_erdwr_attr`: security-extended read/write with attribute information.
- `vnop_select`: vnode-specific readiness notification. This does **not** mean
  every `select` or `poll` call fails: `poll` on an ordinary USFS file returned
  ready in the POWER AIX probe.
- `vfs_statfsvp`: per-vnode filesystem statistics. Ordinary `statvfs` uses the
  implemented filesystem-statistics path and succeeded in the same probe.

The entry-point inventory comes from
[`src/kernel/fs/operations/not_implemented`](../src/kernel/fs/operations/not_implemented/).
Record locking through `fcntl` (`F_GETLK`, `F_SETLK`, `F_SETLKW`) is implemented
by AIX `common_reclock` on each USFS gnode. Locks coordinate users of the same
USFS mount; a mirror's underlying JFS2 path has separate lock state.

The repository's unsupported-operation unit test and vnode compatibility test
check the remaining explicit `ENOSYS` results; the performance test also
measures pathname and descriptor truncation and records current `ioctl`,
record-lock, `fclear`, and FIFO behavior.

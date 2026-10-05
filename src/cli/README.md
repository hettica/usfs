# USFS command-line tools

This directory contains the user-facing administration utility and the device
methods that integrate USFS with the AIX configuration framework.
Use `usfsctl` as the primary user interface for USFS. The other three executables implement the AIX system contract
for device configuration. Do not invoke them directly unless you fully understand what
they do, their prerequisites and effects on device state, and direct invocation is necessary. For device configuration,
use the standard AIX commands: `mkdev` to configure a device, `rmdev` to
unconfigure it, and `chdev` to change its settings.

| Source | Executable | Purpose |
| --- | --- | --- |
| `usfsctl.c` | `usfsctl` | Primary user interface for USFS administration and diagnostics, including status reporting and native AIX tracing. |
| `configure_device.c` | `cfgusfs` | AIX Configure method that brings a USFS device online. |
| `unconfigure_device.c` | `ucfgusfs` | AIX Unconfigure method that takes a USFS device offline. |
| `change_device.c` | `chusfs` | AIX Change method that inspects or adjusts settings for a configured USFS device. |


# AIX coverage archive

Run from a Bash development shell with Git, SSH/SCP, GNU tar and gzip:

```sh
bash collect-coverage.sh root@aix.example.com /ws/usfs-coverage
# Optional local destination and SSH port:
SSH_PORT=22222 bash collect-coverage.sh root@aix.example.com /ws/usfs-coverage ./reports
```

The remote workspace argument is a parent directory. Every invocation creates a
new timestamped child, transfers the current working tree (including nonignored
new files), and keeps source, builds and logs there. Existing workspaces are not
deleted. SSH uses the operator's keys, configuration and known-host verification.
The account must be root and the AIX host must be reserved for USFS tests with no
existing USFS mounts or daemons. The tests install and reload USFS, exercise RPM
install/upgrade/removal, and restore this run's ordinary release build.

All configuration, compilation, tests, collection and reporting execute on AIX.
The host needs the normal AIX project build/package tools, GCC/gcov 13.3.0,
LCOV 1.16 or newer, genhtml, Bash, Perl, tar and gzip. Reporting tools must be
available on the SSH PATH or under /opt/freeware/bin. Set `AIX_PATH` to a
colon-separated search path when reporting tools are installed elsewhere. No local build tools or
local report renderer are needed.

The campaign runs the ordinary `make test` and `make test-e2e` gates, then
`make coverage` with instrumented userspace units, kernel self-tests, every
contract/fault/concurrency script and every E2E script. It also runs the RPM
lifecycle and soak-archive qualification tests, including their bounded soak
campaign and nested/lifecycle journeys. It does not start an operator-selected
long-running soak campaign. Performance benchmarks are never executed.

Packaging qualification must exercise production RPMs, so those release runs do
not add instrumented counters. Configuration-method fault tests also execute in
the ordinary gate because their instrumented counterparts deliberately skip
those cases. Their pass/fail outcomes are retained alongside coverage evidence.
Coverage is optional information; no percentage or threshold decides success.

Coverage metrics include only production files under `src/` and `examples/`.
All diagnostic and test tools live under `tests/tools/`, including `usfs_probe`;
none is installed by the production RPM. Every component report excludes
`tests/` and test-only instrumentation before merging. Under `src/`, the only
excluded paths are `common/instrumentation/config_testing.c`,
`kernel/instrumentation/testing/`, and `kernel/instrumentation/coverage/`.
These implement fault injection, test control, and coverage collection, and are
absent from the production build. Shared production instrumentation, including
`adapter.c`, `hooks.c`, `services.c`, `ras.c`, and `config_normal.c`, remains in
scope. The coverage kernel instruments its compatibility hooks and device ioctl
dispatcher too. Production-code hits from unit tests and kernel self-tests are retained. SonarCloud uses the same exclusions so test infrastructure
does not enter its coverage denominator. Raw profiles remain available as evidence.

The coverage variant instruments `cfgusfs`, `ucfgusfs`, `chusfs`, and `usfsctl`.
Their normal-exit profiles are collected into `coverage/cli.info` and merged
with kernel, client, example, and userspace-unit evidence.

The final transfer downloads `usfs-coverage-<timestamp>-<pid>.tar.gz` into
`out/coverage/` by default. Extract it locally to review or publish selected files:

| Path inside the archive | Content |
|---|---|
| `coverage.lcov` | Merged line, branch and function coverage with repository-relative paths. |
| `sonarqube.xml` | The same line/branch evidence in SonarQube generic coverage format. |
| `coverage/all-html/index.html` | Combined HTML report for local review. |
| `coverage/html/index.html` | Live kernel/client/CLI/example HTML without userspace-unit evidence. |
| `coverage/raw/`, `coverage/graphs/` | Validated gcda/gcno profiles and graphs. |
| `coverage/*.info`, `coverage/manifest.txt` | Component traces, provenance, suite ledger and collection status. |
| `logs/`, `test-results.tsv`, `status.txt` | Build/test logs, exit statuses and overall workflow result. |
| `source.tar.gz` | Exact transferred source snapshot. |
| `CHECKSUMS.sha256` | SHA-256 digest of every archived payload file. |

Upload `coverage.lcov` from an extracted **successful** archive using your service's
uploader and matching Git revision. Codecov accepts LCOV:
https://docs.codecov.com/docs/supported-report-formats

For SonarQube, set `sonar.coverageReportPaths` to the extracted `sonarqube.xml`
while analyzing the matching checkout. Paths are relative to that checkout's
root, independent of the AIX workspace:
https://docs.sonarsource.com/sonarqube-server/analyzing-source-code/test-coverage/generic-test-data

The script does not upload to an external service or require service credentials.
HTML is self-contained and can be reviewed after extraction without access to AIX.

Failures retain a nonzero exit status. When possible, the archive still downloads
with logs and a clearly marked partial report; missing reports remain missing.
Check `workflow_exit_status=0` in `status.txt` and `complete` in
`coverage/report-status.txt` before publishing. An archive's presence alone does
not mean the tests passed. Remote run directories remain available for diagnosis.

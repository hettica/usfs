$ Copyright (c) 2026 Raman Dzehtsiar
$ SPDX-License-Identifier: MIT

$quote "
$set 1
1 "User-space filesystem (USFS)"
$set 2
1 "USFS daemon request timed out"
2 "USFS daemon protocol violation"
3 "USFS daemon exited with active mounts"
4 "USFS forced recovery retained resources"
5 "USFS kernel extension cleanup required"
$set 3
1 "User-space filesystem service failure"
$set 4
1 "Inspect usfsctl and restart the affected daemon and mount"
2 "Inspect errpt and complete USFS device recovery"
$set 5
1 "USFS fault code"
2 "Error number"
3 "Daemon channel"
4 "Protocol opcode"
5 "Request unique identifier"
6 "Affected mount count"

#!/usr/bin/ksh
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Config_Rules method: emit the persistent USFS CuDv name for cfgmgr.

/usr/sbin/lsdev -l usfs0 -F name 2>/dev/null
exit 0

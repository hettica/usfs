#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose: Assemble the release-matched long-running AIX soak archive.
# Usage: bash scripts/aix/build-soak.sh source build version release revision rpm_file aix_version

set -eu

source_root=$1
build_root=$2
version=$3
release=$4
revision=$5
rpm_file=$6
aix_version=$7
name="usfs-soak-$version-$revision-aix$aix_version-ppc64"
stage_root="$build_root/soak-stage"
stage="$stage_root/$name"
packages="$build_root/packages"

rm -rf "$stage_root"
mkdir -p "$stage/bin" "$stage/conf" "$stage/lib" "$stage/rpm" "$stage/scenarios"
cp "$source_root/tests/soak/script.sh" "$source_root/tests/soak/lifecycle.sh" \
    "$source_root/tests/soak/nested-mount.sh" "$source_root/README.md" "$stage/"
cp "$source_root/tests/soak/conf/defaults.conf" "$stage/conf/"
cp "$source_root/tests/soak/lib/common.ksh" "$stage/lib/"
cp "$source_root"/tests/soak/scenarios/*.scn "$stage/scenarios/"
cp "$build_root/bin/usfs_mirror" \
    "$build_root/bin/usfs_memfs" \
    "$build_root/bin/usfsctl" "$build_root/soak-bin/usfs_scenario_daemon" \
    "$build_root/soak-bin/usfs_io_probe" "$build_root/soak-bin/usfs_soak_worker" \
    "$build_root/soak-bin/usfs_admin_probe" "$stage/bin/"
cp "$source_root/scripts/aix/sha256.pl" "$stage/bin/sha256.pl"
cp "$packages/$rpm_file" "$packages/$rpm_file.sha256" "$stage/rpm/"

{
    echo "RPM_FILE=rpm/$rpm_file"
    echo "RPM_QUERY=usfs-$version-$release.ppc"
    echo "PACKAGE_VERSION=$version"
    echo "PACKAGE_RELEASE=$release"
    echo "BUILD_REVISION=$revision"
    echo "TARGET=aix$aix_version-ppc64"
} >"$stage/manifest.txt"

chmod 555 "$stage/script.sh" "$stage/lifecycle.sh" "$stage/nested-mount.sh" \
    "$stage"/bin/*
(cd "$stage" && perl bin/sha256.pl README.md manifest.txt script.sh \
    lifecycle.sh nested-mount.sh bin/* conf/* lib/* rpm/* scenarios/* \
    >CHECKSUMS.sha256)
rm -f "$packages/$name.tar.gz" "$packages/$name.tar.gz.sha256"
(cd "$stage_root" && tar -cf - "$name" | gzip -9 >"$packages/$name.tar.gz")
perl "$source_root/scripts/aix/sha256.pl" "$packages/$name.tar.gz" \
    >"$packages/$name.tar.gz.sha256"

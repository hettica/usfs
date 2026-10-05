#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Purpose: Build the production AIX RPM from a CMake build tree.
# Usage: bash scripts/aix/build-package.sh source build version release aix_version

set -eu

source_root=$1
build_root=$2
version=$3
release=$4
aix_version=$5
aix_rpm_version=$(rpm -q --qf '%{VERSION}-%{RELEASE}' AIX-rpm)
rpm_file="usfs-$version-$release.aix$aix_version.ppc.rpm"
topdir="$build_root/rpmbuild"
packages="$build_root/packages"

rm -rf "$topdir"
mkdir -p "$topdir/BUILD" "$topdir/BUILDROOT" "$topdir/RPMS" \
    "$topdir/SOURCES" "$topdir/SPECS" "$topdir/SRPMS" "$packages"

rpmbuild -bb --nodeps "$source_root/packaging/aix/rpm/usfs.spec" \
    --target "ppc-ibm-aix$aix_version" \
    --define "_topdir $topdir" \
    --define "_binary_payload w1.gzdio" \
    --define "usfs_source_root $source_root" \
    --define "usfs_build_root $build_root" \
    --define "usfs_version $version" \
    --define "usfs_release $release" \
    --define "usfs_aix_rpm_version $aix_rpm_version"

cp "$topdir/RPMS/ppc/$rpm_file" "$packages/$rpm_file"

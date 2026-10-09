#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose: Build and verify the production AIX RPM from a CMake build tree.
# Usage: bash scripts/aix/build-package.sh source build version release revision dirty pre_fault extra_requires aix_version

set -eu

source_root=$1
build_root=$2
version=$3
release=$4
revision=$5
dirty=$6
pre_fault=$7
extra_requires=$8
aix_version=$9
aix_rpm_version=$(rpm -q --qf '%{VERSION}-%{RELEASE}' AIX-rpm)
rpm_file="usfs-$version-$release.aix$aix_version.ppc.rpm"
topdir="$build_root/rpmbuild"
packages="$build_root/packages"
source_archive="$topdir/SOURCES/usfs-source.tar.gz"

if [ -e "$source_root/src/client/include/fuse.h" ] ||
   [ -e "$source_root/src/client/lib/fuse_aix.c" ]; then
    echo "Remove obsolete FUSE sources before packaging the MIT-only native distribution" >&2
    exit 1
fi

rm -rf "$topdir"
mkdir -p "$topdir/BUILD" "$topdir/BUILDROOT" "$topdir/RPMS" \
    "$topdir/SOURCES" "$topdir/SPECS" "$topdir/SRPMS" "$packages"

# Preserve the actual build inputs, including local development changes.
(
    cd "$source_root"
    tar -cf "$topdir/SOURCES/usfs-source.tar" \
        LICENSE README.md .gitignore .clang-format CMakeLists.txt \
        cmake examples packaging scripts src tests
)
gzip -c "$topdir/SOURCES/usfs-source.tar" >"$source_archive"
rm -f "$topdir/SOURCES/usfs-source.tar"

rpmbuild -bb --nodeps "$source_root/packaging/aix/rpm/usfs.spec" \
    --target "ppc-ibm-aix$aix_version" \
    --define "usfs_aix_version $aix_version" \
    --define "usfs_aix_rpm_version $aix_rpm_version" \
    --define "_topdir $topdir" \
    --define "_binary_payload w1.gzdio" \
    --define "usfs_source_root $source_root" \
    --define "usfs_build_root $build_root" \
    --define "usfs_source_archive $source_archive" \
    --define "usfs_version $version" \
    --define "usfs_release $release" \
    --define "usfs_build_revision $revision" \
    --define "usfs_build_dirty $dirty" \
    --define "usfs_pre_fault $pre_fault" \
    --define "usfs_extra_requires $extra_requires"

cp "$topdir/RPMS/ppc/$rpm_file" "$packages/$rpm_file"
perl "$source_root/scripts/aix/verify-rpm.pl" \
    "$packages/$rpm_file" "$version" "$release" \
    /usr/lib/drivers/usfs/usfs.kext \
    /usr/lib/methods/usfs/package_lifecycle \
    /usr/sbin/usfsctl \
    /usr/lib/libusfs.a \
    /usr/include/usfs/usfs.h \
    /usr/lib/pkgconfig/usfs.pc \
    /usr/share/doc/usfs/build-manifest.txt \
    /usr/share/doc/usfs/LICENSE \
    /usr/share/doc/usfs/source.tar.gz
perl "$source_root/scripts/aix/sha256.pl" "$packages/$rpm_file" \
    >"$packages/$rpm_file.sha256"

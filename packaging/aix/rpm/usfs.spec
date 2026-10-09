# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

%{!?usfs_aix_version:%define usfs_aix_version 7.2}
%{!?usfs_aix_rpm_version:%define usfs_aix_rpm_version unknown}
%{!?usfs_version:%define usfs_version 0.1.0}
%{!?usfs_release:%define usfs_release 1}
%{!?usfs_source_root:%define usfs_source_root .}
%{!?usfs_build_root:%define usfs_build_root .}
%{!?usfs_source_archive:%define usfs_source_archive usfs-source.tar.gz}
%{!?usfs_build_revision:%define usfs_build_revision unknown}
%{!?usfs_build_dirty:%define usfs_build_dirty unknown}
%{!?usfs_pre_fault:%define usfs_pre_fault none}
%{!?usfs_extra_requires:%define usfs_extra_requires none}

Name:           usfs
Version:        %{usfs_version}
Release:        %{usfs_release}
Summary:        AIX user-space filesystem kernel extension and runtime
License:        MIT
ExclusiveOS:    aix%{usfs_aix_version}
ExclusiveArch:  ppc
Requires:       AIX-rpm = %{usfs_aix_rpm_version}
%if "%{usfs_extra_requires}" != "none"
Requires:       %{usfs_extra_requires}
%endif

%description
USFS provides an AIX kernel extension, ODM configuration methods, a native
static client library and headers, and native example filesystem daemons.

%prep

%build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/usr/lib/drivers/usfs
mkdir -p %{buildroot}/usr/lib/methods/usfs
mkdir -p %{buildroot}/usr/lib/objrepos
mkdir -p %{buildroot}/usr/lib/nls/msg/en_US
mkdir -p %{buildroot}/usr/lib/ras
mkdir -p %{buildroot}/usr/sbin
mkdir -p %{buildroot}/usr/share/doc/usfs
mkdir -p %{buildroot}/usr/include/usfs
mkdir -p %{buildroot}/usr/lib/pkgconfig

cp %{usfs_build_root}/bin/usfs.kext %{buildroot}/usr/lib/drivers/usfs/usfs.kext
cp %{usfs_build_root}/bin/cfgusfs %{buildroot}/usr/lib/methods/usfs/cfgusfs
cp %{usfs_build_root}/bin/ucfgusfs %{buildroot}/usr/lib/methods/usfs/ucfgusfs
cp %{usfs_build_root}/bin/chusfs %{buildroot}/usr/lib/methods/usfs/chusfs
cp %{usfs_source_root}/packaging/aix/rpm/package_lifecycle.sh %{buildroot}/usr/lib/methods/usfs/package_lifecycle
cp %{usfs_source_root}/packaging/aix/rpm/boot_rule.sh %{buildroot}/usr/lib/methods/usfs/boot_rule
cp %{usfs_source_root}/packaging/aix/odm/usfs.pddv.add %{buildroot}/usr/lib/objrepos/usfs.pddv.add
cp %{usfs_source_root}/packaging/aix/odm/usfs.config_rules.add %{buildroot}/usr/lib/objrepos/usfs.config_rules.add
cp %{usfs_source_root}/packaging/aix/odm/usfs.err %{buildroot}/usr/lib/objrepos/usfs.err
cp %{usfs_source_root}/packaging/aix/odm/usfs.err.undo %{buildroot}/usr/lib/objrepos/usfs.err.undo
cp %{usfs_source_root}/packaging/aix/trace/usfs.trcfmt %{buildroot}/usr/lib/ras/usfs.trcfmt
cp %{usfs_source_root}/packaging/aix/trace/usfs_trace.trc %{buildroot}/usr/lib/ras/usfs_trace.trc
cp %{usfs_source_root}/packaging/aix/trace/usfs_trace_remove.trc %{buildroot}/usr/lib/ras/usfs_trace_remove.trc
gencat %{buildroot}/usr/lib/nls/msg/en_US/usfs.cat %{usfs_source_root}/packaging/aix/odm/usfs.msg

cp %{usfs_build_root}/bin/usfs_mirror %{buildroot}/usr/sbin/usfs_mirror
cp %{usfs_build_root}/bin/usfs_memfs %{buildroot}/usr/sbin/usfs_memfs
cp %{usfs_build_root}/bin/usfs_probe %{buildroot}/usr/sbin/usfs_probe
cp %{usfs_build_root}/bin/usfsctl %{buildroot}/usr/sbin/usfsctl
cp %{usfs_build_root}/bin/chusfs %{buildroot}/usr/sbin/chusfs
cp %{usfs_build_root}/bin/libusfs.a %{buildroot}/usr/lib/libusfs.a
cp %{usfs_source_root}/src/client/include/usfs/usfs.h %{buildroot}/usr/include/usfs/usfs.h
cp %{usfs_build_root}/generated/usfs.pc %{buildroot}/usr/lib/pkgconfig/usfs.pc
cp %{usfs_source_root}/LICENSE %{buildroot}/usr/share/doc/usfs/LICENSE
cp %{usfs_source_archive} %{buildroot}/usr/share/doc/usfs/source.tar.gz

chmod 550 %{buildroot}/usr/lib/methods/usfs/*
chmod 550 %{buildroot}/usr/lib/drivers/usfs/usfs.kext
chmod 555 %{buildroot}/usr/sbin/*
chmod 444 %{buildroot}/usr/lib/objrepos/usfs.pddv.add
chmod 444 %{buildroot}/usr/lib/objrepos/usfs.err %{buildroot}/usr/lib/objrepos/usfs.err.undo
chmod 444 %{buildroot}/usr/lib/nls/msg/en_US/usfs.cat
chmod 444 %{buildroot}/usr/lib/ras/usfs.trcfmt %{buildroot}/usr/lib/ras/usfs_trace.trc %{buildroot}/usr/lib/ras/usfs_trace_remove.trc
chmod 444 %{buildroot}/usr/lib/libusfs.a %{buildroot}/usr/include/usfs/usfs.h \
    %{buildroot}/usr/lib/pkgconfig/usfs.pc %{buildroot}/usr/share/doc/usfs/LICENSE \
    %{buildroot}/usr/share/doc/usfs/source.tar.gz

{
    echo "package_version=%{version}-%{release}"
    echo "build_revision=%{usfs_build_revision}"
    echo "worktree_dirty=%{usfs_build_dirty}"
    echo "target=aix-%{usfs_aix_version}-ppc64"
    echo "payload_sha256_begin"
    cd %{buildroot}
    /usr/bin/perl %{usfs_source_root}/scripts/aix/sha256.pl \
        usr/lib/drivers/usfs/usfs.kext \
        usr/lib/methods/usfs/cfgusfs \
        usr/lib/methods/usfs/ucfgusfs \
        usr/lib/methods/usfs/chusfs \
        usr/sbin/usfs_mirror usr/sbin/usfs_memfs \
        usr/sbin/usfs_probe usr/sbin/usfsctl \
        usr/sbin/chusfs usr/lib/objrepos/usfs.err \
        usr/lib/objrepos/usfs.err.undo usr/lib/ras/usfs.trcfmt \
        usr/lib/ras/usfs_trace.trc usr/lib/ras/usfs_trace_remove.trc \
        usr/lib/libusfs.a usr/include/usfs/usfs.h usr/lib/pkgconfig/usfs.pc \
        usr/share/doc/usfs/LICENSE usr/share/doc/usfs/source.tar.gz
    echo "payload_sha256_end"
} > %{buildroot}/usr/share/doc/usfs/build-manifest.txt
chmod 444 %{buildroot}/usr/share/doc/usfs/build-manifest.txt

%pre -p /usr/bin/ksh
# This check belongs to the incoming RPM: an older installed lifecycle helper
# may incorrectly treat Defined as proof that the kernel has been unloaded.
if [ -x /usr/lib/methods/usfs/ucfgusfs ]; then
    /usr/lib/methods/usfs/ucfgusfs -l usfs0 || exit 1
else
    records=$(/usr/bin/odmget -q 'name=usfs0' CuDv) || {
        echo "usfs: cannot verify a fresh installation; repair ODM access and retry before replacing USFS files" >&2
        exit 1
    }
    if [ -n "$records" ] || [ -e /usr/lib/drivers/usfs/usfs.kext ] ||
       [ -L /usr/lib/drivers/usfs/usfs.kext ] || [ -e /dev/usfs0 ] ||
       [ -L /dev/usfs0 ]; then
        echo "usfs: teardown method is unavailable; restore the installed ucfgusfs and complete cleanup before retrying" >&2
        exit 1
    fi
fi
if [ "%{usfs_pre_fault}" = after-unconfigure ]; then
    echo "usfs: injected package interruption after unconfigure" >&2
    exit 97
fi
exit 0

%post -p /usr/bin/ksh
/usr/lib/methods/usfs/package_lifecycle post-install

%preun -p /usr/bin/ksh
if [ "$1" -eq 0 ]; then
    /usr/lib/methods/usfs/package_lifecycle pre-erase || exit 1
fi
exit 0

%files
%defattr(-,root,system,-)
%dir %attr(0550,root,system) /usr/lib/drivers/usfs
%attr(0550,root,system) /usr/lib/drivers/usfs/usfs.kext
%dir %attr(0550,root,system) /usr/lib/methods/usfs
%attr(0550,root,system) /usr/lib/methods/usfs/cfgusfs
%attr(0550,root,system) /usr/lib/methods/usfs/ucfgusfs
%attr(0550,root,system) /usr/lib/methods/usfs/chusfs
%attr(0550,root,system) /usr/lib/methods/usfs/package_lifecycle
%attr(0550,root,system) /usr/lib/methods/usfs/boot_rule
%attr(0444,root,system) /usr/lib/objrepos/usfs.pddv.add
%attr(0444,root,system) /usr/lib/objrepos/usfs.config_rules.add
%attr(0444,root,system) /usr/lib/objrepos/usfs.err
%attr(0444,root,system) /usr/lib/objrepos/usfs.err.undo
%attr(0444,root,system) /usr/lib/nls/msg/en_US/usfs.cat
%attr(0444,root,system) /usr/lib/ras/usfs.trcfmt
%attr(0444,root,system) /usr/lib/ras/usfs_trace.trc
%attr(0444,root,system) /usr/lib/ras/usfs_trace_remove.trc
%attr(0555,root,system) /usr/sbin/usfs_mirror
%attr(0555,root,system) /usr/sbin/usfs_memfs
%attr(0555,root,system) /usr/sbin/usfs_probe
%attr(0555,root,system) /usr/sbin/usfsctl
%attr(0555,root,system) /usr/sbin/chusfs
%attr(0444,root,system) /usr/lib/libusfs.a
%attr(0444,root,system) /usr/lib/pkgconfig/usfs.pc
%dir %attr(0555,root,system) /usr/include/usfs
%attr(0444,root,system) /usr/include/usfs/usfs.h
%dir %attr(0555,root,system) /usr/share/doc/usfs
%attr(0444,root,system) /usr/share/doc/usfs/LICENSE
%attr(0444,root,system) /usr/share/doc/usfs/source.tar.gz
%attr(0444,root,system) /usr/share/doc/usfs/build-manifest.txt

%changelog
* Mon Aug 03 2026 USFS maintainers - 0.1.0-1
- Initial versioned AIX RPM lifecycle.

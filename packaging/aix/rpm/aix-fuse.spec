# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

%{!?aix_fuse_aix_version:%define aix_fuse_aix_version 7.2}
%{!?aix_fuse_aix_rpm_version:%define aix_fuse_aix_rpm_version unknown}
%{!?aix_fuse_version:%define aix_fuse_version 0.1.0}
%{!?aix_fuse_release:%define aix_fuse_release 1}

Name:           aix-fuse
Version:        %{aix_fuse_version}
Release:        %{aix_fuse_release}
Summary:        FUSE interface adapter for USFS
License:        MIT AND LGPL-2.1-only
ExclusiveOS:    aix%{aix_fuse_aix_version}
ExclusiveArch:  ppc
Requires:       AIX-rpm = %{aix_fuse_aix_rpm_version}

%description
The aix-fuse SDK provides a FUSE-compatible static library, public headers,
and two example filesystems. Native USFS client objects are embedded in the
archive. Mounting requires a compatible configured USFS kernel extension.
This package does not install or configure the kernel extension.

%prep

%build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/usr/lib/pkgconfig %{buildroot}/usr/include/fuse3
mkdir -p %{buildroot}/usr/sbin %{buildroot}/usr/share/doc/aix-fuse
cp %{aix_fuse_build_root}/bin/libfuse3.a %{buildroot}/usr/lib/libfuse3.a
cp %{aix_fuse_build_root}/generated/fuse3.pc %{buildroot}/usr/lib/pkgconfig/fuse3.pc
cp %{aix_fuse_source_root}/include/*.h %{buildroot}/usr/include/fuse3/
cp %{aix_fuse_build_root}/bin/aix_fuse_mirror %{buildroot}/usr/sbin/aix_fuse_mirror
cp %{aix_fuse_build_root}/bin/aix_fuse_memfs %{buildroot}/usr/sbin/aix_fuse_memfs
cp %{aix_fuse_source_root}/README.md %{buildroot}/usr/share/doc/aix-fuse/README.md
cp %{aix_fuse_source_root}/LICENSE %{buildroot}/usr/share/doc/aix-fuse/LICENSE
cp %{aix_fuse_source_root}/LICENSE.MIT %{buildroot}/usr/share/doc/aix-fuse/LICENSE.MIT
cp %{aix_fuse_source_root}/LICENSE.LGPL-2.1-only %{buildroot}/usr/share/doc/aix-fuse/LICENSE.LGPL-2.1-only
cp %{aix_fuse_native_root}/LICENSE %{buildroot}/usr/share/doc/aix-fuse/LICENSE.native-usfs
cp %{aix_fuse_source_stage}/source-provenance.txt %{buildroot}/usr/share/doc/aix-fuse/source-provenance.txt
cp %{aix_fuse_source_stage}/adapter-source-manifest.txt %{buildroot}/usr/share/doc/aix-fuse/adapter-source-manifest.txt
cp %{aix_fuse_source_stage}/native-source-manifest.txt %{buildroot}/usr/share/doc/aix-fuse/native-source-manifest.txt
cp %{aix_fuse_source_archive} %{buildroot}/usr/share/doc/aix-fuse/source.tar.gz
chmod 555 %{buildroot}/usr/sbin/aix_fuse_mirror %{buildroot}/usr/sbin/aix_fuse_memfs
chmod 444 %{buildroot}/usr/lib/libfuse3.a %{buildroot}/usr/lib/pkgconfig/fuse3.pc
chmod 444 %{buildroot}/usr/include/fuse3/*.h %{buildroot}/usr/share/doc/aix-fuse/*
{
    echo "package_version=%{version}-%{release}"
    echo "target=aix-%{aix_fuse_aix_version}-ppc64"
    cat %{aix_fuse_source_stage}/source-provenance.txt
    echo "payload_sha256_begin"
    cd %{buildroot}
    /usr/bin/perl %{aix_fuse_source_root}/scripts/aix/sha256.pl \
        usr/lib/libfuse3.a usr/lib/pkgconfig/fuse3.pc usr/include/fuse3/*.h \
        usr/sbin/aix_fuse_mirror usr/sbin/aix_fuse_memfs \
        usr/share/doc/aix-fuse/LICENSE usr/share/doc/aix-fuse/LICENSE.MIT \
        usr/share/doc/aix-fuse/LICENSE.LGPL-2.1-only \
        usr/share/doc/aix-fuse/LICENSE.native-usfs \
        usr/share/doc/aix-fuse/source.tar.gz \
        usr/share/doc/aix-fuse/source-provenance.txt \
        usr/share/doc/aix-fuse/adapter-source-manifest.txt \
        usr/share/doc/aix-fuse/native-source-manifest.txt
    echo "payload_sha256_end"
} >%{buildroot}/usr/share/doc/aix-fuse/build-manifest.txt
chmod 444 %{buildroot}/usr/share/doc/aix-fuse/build-manifest.txt

%files
%defattr(-,root,system,-)
%attr(0444,root,system) /usr/lib/libfuse3.a
%attr(0444,root,system) /usr/lib/pkgconfig/fuse3.pc
%dir %attr(0555,root,system) /usr/include/fuse3
%attr(0444,root,system) /usr/include/fuse3/*.h
%attr(0555,root,system) /usr/sbin/aix_fuse_mirror
%attr(0555,root,system) /usr/sbin/aix_fuse_memfs
%dir %attr(0555,root,system) /usr/share/doc/aix-fuse
%attr(0444,root,system) /usr/share/doc/aix-fuse/*

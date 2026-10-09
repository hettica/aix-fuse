#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Purpose: Package the adapter SDK with its exact native source inputs.
# Usage: bash scripts/aix/build-package.sh source build native version release revision dirty native_revision native_dirty source_kind aix_version

set -eu

source_root=$1
build_root=$2
native_root=$3
version=$4
release=$5
revision=$6
dirty=$7
native_revision=$8
native_dirty=$9
source_kind=${10}
aix_version=${11}
aix_rpm_version=$(rpm -q --qf '%{VERSION}-%{RELEASE}' AIX-rpm)
rpm_file="aix-fuse-$version-$release.aix$aix_version.ppc.rpm"
topdir="$build_root/rpmbuild"
packages="$build_root/packages"
source_stage="$topdir/SOURCES/aix-fuse-source"
source_archive="$topdir/SOURCES/aix-fuse-source.tar.gz"

rm -rf "$topdir"
mkdir -p "$topdir/BUILD" "$topdir/BUILDROOT" "$topdir/RPMS" \
    "$topdir/SOURCES" "$topdir/SPECS" "$topdir/SRPMS" "$packages" \
    "$source_stage/bundled/usfs/src/client/include/usfs" \
    "$source_stage/bundled/usfs/src/client/lib" \
    "$source_stage/bundled/usfs/src/common"

for file in LICENSE LICENSE.MIT LICENSE.LGPL-2.1-only README.md \
        CMakeLists.txt .gitignore .clang-format; do
    cp "$source_root/$file" "$source_stage/$file"
done
for directory in include src examples tests scripts packaging; do
    cp -R "$source_root/$directory" "$source_stage/$directory"
done

# Copy a complete, explicit native component. The development checkout can
# still contain obsolete FUSE files while extraction is being verified.
cp "$native_root/LICENSE" "$source_stage/bundled/usfs/LICENSE"
cp "$native_root/LICENSE" "$source_stage/LICENSE.native-usfs"
cp "$native_root/src/client/CMakeLists.txt" "$native_root/src/client/usfs.pc.in" \
    "$source_stage/bundled/usfs/src/client/"
cp "$native_root/src/client/include/usfs/usfs.h" \
    "$source_stage/bundled/usfs/src/client/include/usfs/"
cp "$native_root/src/client/lib/usfs_client.c" \
    "$source_stage/bundled/usfs/src/client/lib/"
cp "$native_root/src/common/usfs_validate.c" "$native_root"/src/common/*.h \
    "$source_stage/bundled/usfs/src/common/"

(
    cd "$source_stage/bundled/usfs"
    find LICENSE src -type f -print | LC_ALL=C sort |
        while IFS= read -r file; do
            perl "$source_root/scripts/aix/sha256.pl" "$file"
        done
) >"$source_stage/native-source-manifest.txt"
(
    cd "$source_stage"
    find CMakeLists.txt LICENSE LICENSE.MIT LICENSE.LGPL-2.1-only LICENSE.native-usfs \
        README.md .gitignore .clang-format include src examples tests scripts packaging -type f -print |
        LC_ALL=C sort |
        while IFS= read -r file; do
            perl "$source_root/scripts/aix/sha256.pl" "$file"
        done
) >"$source_stage/adapter-source-manifest.txt"
{
    echo "adapter_revision=$revision"
    echo "adapter_dirty=$dirty"
    echo "native_revision=$native_revision"
    echo "native_dirty=$native_dirty"
    echo "native_source_kind=$source_kind"
    echo "source_hashes_begin"
    (cd "$source_stage" && perl scripts/aix/sha256.pl \
        adapter-source-manifest.txt native-source-manifest.txt)
    echo "source_hashes_end"
} >"$source_stage/source-provenance.txt"
(
    cd "$topdir/SOURCES"
    tar -cf aix-fuse-source.tar aix-fuse-source
)
gzip -c "$topdir/SOURCES/aix-fuse-source.tar" >"$source_archive"
rm -f "$topdir/SOURCES/aix-fuse-source.tar"

rpmbuild -bb --nodeps "$source_root/packaging/aix/rpm/aix-fuse.spec" \
    --define "_topdir $topdir" \
    --define "_binary_payload w1.gzdio" \
    --define "aix_fuse_source_root $source_root" \
    --define "aix_fuse_build_root $build_root" \
    --define "aix_fuse_native_root $native_root" \
    --define "aix_fuse_source_stage $source_stage" \
    --define "aix_fuse_source_archive $source_archive" \
    --define "aix_fuse_version $version" \
    --define "aix_fuse_release $release" \
    --define "aix_fuse_aix_version $aix_version" \
    --define "aix_fuse_aix_rpm_version $aix_rpm_version"

cp "$topdir/RPMS/ppc/$rpm_file" "$packages/$rpm_file"
perl "$source_root/scripts/aix/verify-rpm.pl" \
    "$packages/$rpm_file" "$version" "$release" "$aix_version" \
    /usr/lib/libfuse3.a /usr/lib/pkgconfig/fuse3.pc \
    /usr/include/fuse3/fuse.h /usr/include/fuse3/fuse_common.h \
    /usr/include/fuse3/fuse_opt.h /usr/include/fuse3/fuse_log.h \
    /usr/include/fuse3/libfuse_config.h \
    /usr/sbin/aix_fuse_mirror /usr/sbin/aix_fuse_memfs \
    /usr/share/doc/aix-fuse/LICENSE /usr/share/doc/aix-fuse/LICENSE.MIT \
    /usr/share/doc/aix-fuse/LICENSE.LGPL-2.1-only \
    /usr/share/doc/aix-fuse/LICENSE.native-usfs \
    /usr/share/doc/aix-fuse/source.tar.gz \
    /usr/share/doc/aix-fuse/build-manifest.txt
perl "$source_root/scripts/aix/sha256.pl" "$packages/$rpm_file" \
    >"$packages/$rpm_file.sha256"

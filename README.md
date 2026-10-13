# aix-fuse

FUSE-compatible userspace filesystem adapter for IBM AIX, powered by [USFS](https://github.com/hettica/usfs).

## Current project state

Today it's a technical preview. The core functionality works, with known issues.
You can run the provided examples or conduct your own experiments.
Do not use this preview in production. Kernel extension failures can crash the system.
Upgrade from this version will not be supported.
Before porting a backend, check the [current FUSE compatibility guide](docs/compatibility.md).

## Build

Build directly on AIX with GCC, CMake 3.20 or newer, Git, Unix Makefiles, Bash, Perl,
and RPM build tools. The SDK contains 64-bit static objects. RPM filenames and
metadata use the build host's AIX version, and the RPM requires that host's exact
`AIX-rpm` version-release. Build on AIX 7.2 to produce an AIX 7.2 package.

Configuration downloads the native client from `https://github.com/hettica/usfs.git`
at the pinned commit `d888e9574addbd51b757770706381a1d0a594f45`:

```sh
cmake -S . -B build
cd build
make package
```

RPMs and their SHA-256 checksums appear under
`build/packages`. 

For local development or offline sources, pass
`-DAIX_FUSE_USFS_SOURCE_DIR=/path/to/native-usfs` when configuring. This override
takes precedence over the download. `AIX_FUSE_USFS_REVISION` can select another compatible published 40-character
commit SHA; branch names and moving tags are not accepted.
`AIX_FUSE_USFS_REPOSITORY` overrides the repository URL. When updating an
existing build directory, set the desired revision explicitly or use a fresh
build directory, since CMake retains cached dependency settings.

## Install and develop

The RPM installs `/usr/lib/libfuse3.a`, `/usr/include/fuse3`, `fuse3.pc`,
`aix_fuse_mirror`, and `aix_fuse_memfs`. It has no dependency on a separate USFS
RPM. Mounting still requires a compatible configured USFS kernel extension,
which can be installed separately with the USFS RPM. Missing device,
permission, and protocol errors are reported at runtime. This adapter
embeds USFS client API 4 and requires a kernel extension using protocol 12.

USFS requires atomic object checks for `unlink`, `rmdir`, and `rename`.
Filesystems must supply the optional `*_checked` callbacks to enable
these operations. Ordinary pathname FUSE callbacks cannot satisfy that
contract. Mapped I/O requires the optional `pager_*` callbacks, which
retain one resource per file object through close, rename, and unlink.
Directory listing receives no per-open handle: `readdir` gets NULL `fi`
and can use `fuse_get_context()->object_identity`.

Run as root on an isolated, recoverable AIX host with a compatible USFS kernel
already configured. Run the installation command from this repository's root.
The examples run in the foreground; use a separate session to access and
unmount each filesystem.

```sh
# Install the adapter and check USFS status
rpm -ivh "build/packages/aix-fuse-0.1.0-1.aix$(uname -v).$(uname -r).ppc.rpm"
usfsctl

### Run the memfs example
# Main session
mkdir -p /mnt/aix-fuse-mem
aix_fuse_memfs /mnt/aix-fuse-mem

# Separate session
printf 'AIX FUSE works\n' >/mnt/aix-fuse-mem/hello.txt
cat /mnt/aix-fuse-mem/hello.txt
umount /mnt/aix-fuse-mem

### Run the read-only mirror example
# Main session
mkdir -p /mnt/aix-fuse-mirror
aix_fuse_mirror --source=/usr /mnt/aix-fuse-mirror

# Separate session
ls /mnt/aix-fuse-mirror
umount /mnt/aix-fuse-mirror
```

The adapter embeds native client objects, so applications do not link
a separate native library. Updating an installed USFS RPM does not update these
embedded objects. Rebuild aix-fuse and relink applications for relevant native
client fixes. 

## Licensing

This repository contains MIT and LGPL-2.1-only components; see `LICENSE` for
the component boundaries and full notice.

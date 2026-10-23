# FUSE compatibility on AIX

This guide helps you decide whether an existing **high-level libfuse 3** filesystem can run through `aix-fuse`. It describes the adapter as it works today, including callbacks declared in its header but not called. It covers source-level API behavior, not Linux binary compatibility or a production-readiness guarantee.

**Version scope:** These are limitations of the adapter and pinned USFS in their current state, not a claim that missing callbacks can never be supported on AIX. Future versions are intended to improve compatibility. Some AIX call contracts may still require backend changes, so check the guide for the version you use.

**Quick answer:** An ordinary file backend can work if its permission policy agrees with AIX mode bits, its directory listing is a complete restartable listing, and it does not depend on a particular open handle for vnode-wide operations. Deletion and rename currently require AIX-specific checked callbacks. Memory mapping requires AIX-specific pager callbacks. Several other libfuse callbacks cannot be delivered by the pinned USFS implementation at all. Check the lists below before linking an existing backend.

## Decide before porting

| Your backend needs… | Current result | What to do |
| --- | --- | --- |
| Ordinary metadata, `open`, uncached `read` and `write` | Available, with the conditions below | Check permissions, paths, flags and lifecycle assumptions. |
| `unlink`, `rmdir` or `rename` through the original pathname callbacks alone | **Unavailable now**; those callbacks are not called | Implement `unlink_checked`, `rmdir_checked` and/or `rename_checked` with atomic identity checks. |
| `mmap` or cached page I/O | **Unavailable without pager extensions**; mapping fails with `ENOSYS` | Implement the six object-scoped `pager_*` callbacks, or reject mapping. |
| A directory cursor or `fi->fh` inside `readdir` | **Incompatible with current listing behavior** | Generate a complete, repeatable listing from the directory object; `fi` is `NULL`. |
| Backend-specific access decisions through `.access` | **Unavailable** | Express permissions through mode/owner information that AIX can enforce; a policy requiring `.access` needs USFS changes. |
| Extended attributes, file locks, custom `ioctl`, `poll`, special nodes, or another callback in [group 3](#3-current-usfs-cannot-deliver-these-callbacks) | **Unavailable through current USFS** | Do not rely on those callbacks for this port. |
| `read_buf` or `write_buf` instead of ordinary `read` or `write` | **Not implemented by the adapter** | Supply ordinary `read`/`write` callbacks for now. |

A callback being present in `struct fuse_operations` does **not** mean AIX calls it. The adapter implements a subset of the upstream high-level API. The groups below classify every one of the 44 high-level callbacks in the reviewed libfuse snapshot exactly once.

## Rules that affect several callbacks

### Permissions and paths

USFS checks Unix mode bits, ownership and AIX privileges in the kernel before several requests reach the daemon. For example, a backend cannot grant Alice access in `.open` when the file attributes it reported make AIX deny Alice first. Make the reported attributes agree with the backend's intended access policy. The `.access` callback is not called.

The client reconstructs paths from aliases. A path can become `NULL` after unlink, and `nullpath_ok` can omit a known path. If another writer changes the backing namespace, a path can also refer to a different object than the vnode AIX selected. For callbacks that use names, control those writers or resolve the selected object by stable identity where the API supplies it. Checked namespace mutations have a stricter atomic rule described below.

### Open handles, caching and mapping

Ordinary uncached `read` and `write` receive the matching open's `fi->fh`. Only `fh` and `flags` are translated between FUSE and USFS open-file structures. The adapter does not implement behavioral effects of `direct_io`, `keep_cache`, `cache_readdir`, `nonseekable`, or `parallel_direct_writes`. Later I/O can receive reconstructed read/write flags rather than the exact original open flags.

Once a VMM segment exists, cached ordinary I/O and page faults can use an **object-scoped pager lease** instead of the opener's `fh` or the ordinary `.read`/`.write` callback. A pager resource must identify the file object and remain valid through close, rename and unlink. A page fault has no reliable caller or per-open identity. If the backend cannot provide that resource and authorize each mapping, it must leave mapping unsupported.

### Directory and close behavior

`readdir` gets offset `0`, `fi == NULL` and `FUSE_READDIR_DEFAULTS`. Return a full listing that can be restarted. USFS caches directory snapshots by node ID, UID and GID, not PID, so two processes with the same UID/GID must see the same listing. A per-open iterator or PID-specific view cannot be made reliable through this callback.

`flush` is tied to AIX file-structure close, which can differ from a callback per descriptor close after `dup` or `fork`. `release` disposes the ordinary open handle; it may run while a mapping remains. `fsync`, `fsyncdir` and some truncations identify the vnode without the opener's `fh`.

## Callback reference

### 1. Full callback compatibility

These callback contracts need no extra backend-specific AIX condition. This does not imply that the rest of the filesystem is fully compatible.

| Callback | Behavior |
| --- | --- |
| `destroy` | Receives the mount private data after client handles and pager resources are disposed. |
| `releasedir` | Receives the matching `opendir` handle for cleanup; its return value is ignored, as in upstream libfuse. A removed directory may have a `NULL` path. |

### 2. Works with the original callback under conditions

These callbacks **are called now**. The listed condition is the backend author's responsibility.

| Callback | Condition to check in your backend |
| --- | --- |
| `getattr` | Accept `fi == NULL`; handle a detached object with `NULL` path when its attributes are needed. Do not depend on unimplemented `fuse_config` cache or inode options. |
| `readlink` | Ensure the pathname still denotes the symlink AIX selected; exclude outside name replacement or resolve the intended object internally. |
| `mkdir` | Align the parent directory's reported permissions with the backend's create policy; create the new name atomically. |
| `symlink` | Apply the same parent permission rule; use the supplied link target verbatim and create the name atomically. |
| `link` | Align parent permissions and keep the source path bound to the selected object while creating the link. |
| `chmod` | Accept `fi == NULL`, possible detached objects and AIX ownership checks. A combined attribute update can partially complete across callbacks. |
| `chown` | Apply the same object and ownership rules; honor `uid_t(-1)`/`gid_t(-1)` as unchanged fields and possible set-ID-bit clearing. |
| `truncate` | Support truncation without an open `fh`, including descriptor truncation: the pinned USFS path can send `fi == NULL`/`fh == 0`. |
| `open` | Make mode bits match the access policy; store per-open state in `fi->fh`. Do not rely on returned cache/direct-I/O flags being enforced. |
| `read` | Use the supplied `fh`, offset and length for uncached I/O; do not infer the original open mode from `fi->flags`. Keep mapped/cached I/O coherent through the pager if mapping is enabled. |
| `write` | Follow the same handle and cache rules as `read`; enforce atomic append or other required serialization in the backend. |
| `statfs` | Return filesystem-wide information independent of the queried file: the callback receives `"/"`. |
| `flush` | Tolerate AIX file-structure-close timing rather than requiring exactly one call per descriptor `close()`. |
| `release` | Clean up the matching ordinary `fh` once; do not rely on its return error being reported or close the only resource needed by a later page fault. |
| `fsync` | Sync by object even when `fi == NULL` and the path is `NULL`; do not require the opener's `fh`. |
| `opendir` | The returned `fh` is available to `releasedir`, but not to `readdir`; align directory permission policy with AIX checks. |
| `readdir` | Produce a complete restartable listing with offset `0` and `fi == NULL`, independent of PID and per-open cursor. Readdir-plus and streaming offsets are unavailable. |
| `fsyncdir` | Sync directory state without the opener's `fh`; handle a detached path when needed. |
| `init` | Use returned private data and only the implemented config options `hard_remove` and `nullpath_ok`. `conn.proto_major/minor` report `3.19`, not the Linux kernel FUSE protocol, and other config/capability fields are inert. |
| `create` | Handle ordinary create-and-open with mode only. Other AIX create activations or initial attributes need `create_attr`. |
| `utimens` | Accept translated `UTIME_NOW`/`UTIME_OMIT`, `fi == NULL`, and possible detached objects; combined attribute changes can be partial. |
| `syncfs` | Sync the whole filesystem; the supplied path is always `"/"`. |

For example, a backend whose `ftruncate` always dereferences `fi->fh` is **not** compatible with the `truncate` row even though the original function prototype compiles. A backend that can find its object independently of that handle can use the callback.

### 3. Current USFS cannot deliver these callbacks

The adapter header declares these names, but the pinned USFS kernel/client does not provide the request or information needed to call them. A backend restriction alone cannot enable the original callback. Related system calls may sometimes work through a different route; that does not activate these callbacks.

| Callback | What is missing for an application |
| --- | --- |
| `mknod` | Special-node and FIFO creation through this callback. |
| `setxattr` | Extended-attribute writes. |
| `getxattr` | Extended-attribute reads. |
| `listxattr` | Extended-attribute enumeration. |
| `removexattr` | Extended-attribute removal. |
| `access` | Backend-controlled `access(2)` decisions; AIX makes its own mode/privilege decision. |
| `lock` | POSIX record-lock requests and lock-owner information. |
| `bmap` | Backend block mapping. |
| `ioctl` | Custom filesystem ioctl requests and arguments. |
| `poll` | Backend poll/select registration and notifications. |
| `flock` | Open-file lock requests. |
| `fallocate` | Guaranteed space allocation/reservation semantics. |
| `copy_file_range` | A single backend range-copy callback; applications may still copy with reads and writes. |
| `lseek` | Callback-based `SEEK_DATA`/`SEEK_HOLE`; ordinary seek is a separate path. |
| `statx` | A statx request and result structure; ordinary stat uses `getattr`. |

### 4. Current adapter gaps

USFS has sufficient routes for these operations, but the **original FUSE callbacks are not called by this adapter today**. Do not treat them as supported just because a function pointer exists in the header.

| Callback | Current behavior and possible path forward |
| --- | --- |
| `unlink` | Only `unlink_checked` registers the operation now. A future pathname bridge would need stable identities and exclusive control of **all** writers to that namespace while checking and deleting the selected entry. |
| `rmdir` | Only `rmdir_checked` registers the operation now. The same atomic identity and namespace control would be required for a pathname bridge. |
| `rename` | Only `rename_checked` registers the operation now. A future bridge would need atomic checks for both selected source and destination (including expected absence), plus hidden-name and rollback cases. |
| `write_buf` | No buffer-vector bridge or complete helper API. Use ordinary `write`; an eventual bridge would inherit its handle, flags and cache conditions. |
| `read_buf` | No buffer-vector bridge or complete helper API. Use ordinary `read`; an eventual bridge would inherit its handle and cache conditions. |

The proposed pathname bridges are **not implemented or verified**. A `stat` followed by pathname deletion does not satisfy the selected-object guarantee when another writer can change the name between those steps.

## Optional AIX-specific callbacks

These are additions to the high-level FUSE API, not requirements imposed on every backend:

| Extension | When it is needed |
| --- | --- |
| `unlink_checked`, `rmdir_checked`, `rename_checked` | Required **today** to perform those mutations. Compare the expected parent and child object identities (or expected destination absence) and mutate atomically under the backend's namespace lock. Reject a mismatch without side effects. |
| `pager_acquire`, `pager_authorize`, `pager_enable_write`, `pager_read`, `pager_write`, `pager_release` | Required for mapping. Acquire one stable object resource, authorize every mapping with that caller's credentials/access, enable writing before writable use, and retain the resource through final segment teardown and in-flight page I/O. Page faults do not carry the opener's handle or caller identity. |
| `create_attr` | Required when the backend must support AIX create activations or initial attributes that ordinary FUSE `create` cannot express. |
| `export_id`, `resolve_id` | Needed for stable AIX fid/vget object tokens; they do not replace ordinary pathname callbacks. |

Extensions do not recover a per-open handle for `readdir`, `fsync` or page faults, and do not activate the callbacks in group 3.

## Example decisions

- A read-only static tree that reports ordinary Unix permissions, lists each directory in one complete pass, and does not need mapping can use the current original callbacks without the mutation or pager extensions. It still must satisfy the conditional rows it implements.
- A mutable filesystem that needs `rm` and `mv` must implement the checked mutation extensions now. If it also needs `mmap`, it needs the pager extension and an object that survives close and unlink.
- A backend whose directory view depends on each open's private iterator, or whose policy must be decided by `.access`, cannot preserve those behaviors simply by recompiling against this adapter.
- A backend that implements only `read_buf`/`write_buf`, or relies on locks, xattrs or custom ioctls, needs different callbacks or additional USFS/adapter implementation before the corresponding operations can work.

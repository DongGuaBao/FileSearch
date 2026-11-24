# Linux service

The Linux implementation runs search, indexing, fanotify and subscriptions in
one process. Windows production builds and cache formats remain separate.

## Build and run on Ubuntu 24.04

```sh
sudo apt-get install build-essential cmake ninja-build python3 libsqlite3-dev nodejs xfsprogs btrfs-progs
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo env HOME=/home/donggua ctest --test-dir build --output-on-failure
sudo install -m 0755 build/FileSearchServiceLinux /usr/local/bin/FileSearchServiceLinux
sudo install -m 0644 deploy/filesearch-linux.service /etc/systemd/system/filesearch-linux.service
sudo systemctl daemon-reload
sudo systemctl enable --now filesearch-linux.service
```

Tests currently use `/home/donggua` for isolated fixtures. Create that directory
before testing on another machine. Tests that mount temporary ext4 filesystems
require mount privileges; a root process in a restricted container is insufficient.
The supplied unit indexes `/`; adjust its first argument to restrict the root.
Load the XFS/Btrfs kernel modules before running their optional image tests
(`sudo modprobe xfs btrfs`), if those filesystems are not built into the kernel.
Tests without the formatter or kernel support report a skip, not a validation.
Its second argument is a persistent, root-owned state/IPC directory. Do not use
`/run` for persistent catalog storage. The directory must not contain the search
root, and is itself excluded from scanning and events. Shutdown uses SIGTERM.

## Filesystem policy

Only ext4, XFS and Btrfs mount types are eligible. Pseudo, network and unsupported
filesystems are excluded. `/mnt/<single lowercase drive letter>` (including
`/mnt/c` and `/mnt/d`) and `/mnt/wslg` are always excluded. Symlinks are indexed
but never traversed. Runtime mount topology is checked once per second; topology
changes cause catalog reconciliation and invalidate subscription history.

Real XFS/Btrfs image tests cover search, pinyin, hardlink identities, raw names,
directory moves, paged snapshots and durable live mutations. Btrfs also covers
equal inode numbers in distinct subvolumes, dynamic subvolume creation,
read-only snapshot creation/deletion, a configured root inside a subvolume and
a subvolume mounted as the search root. Btrfs object keys normalize statfs fsid
to the superblock-root identity while retaining the real subvolume ID and
generation in the kernel FID. When FID-mode fanotify rejects marking a subvolume
path with EXDEV, the service locates the fixed top-level root by a privileged
kernel handle and marks that same filesystem. It does not mount or modify the
filesystem to do so; root capabilities required by `open_by_handle_at` must be
available. A restricted root container can fail this operation.

## Local protocol

The additive [Node service client](../clients/node/README.md) exposes search,
status, cancellable change subscriptions and Linux paged snapshots. It preserves
64-bit IDs/cursors as BigInt and does not require or replace the Windows addon.
Node.js is needed for the optional client tests discovered by CMake.

`search.sock` and `events.sock` are Unix stream sockets with mode 0600. Each
message consists of a 32-bit little-endian byte length and a UTF-8 JSON body.
Frames larger than 1 MiB are rejected. Search supports `query`, `scope_path`
and `max_results`. Matching includes case-insensitive filename substrings,
Chinese pinyin and pinyin initials. `command: "status"` returns readiness,
the event cursor and the observed kernel queue overflow count.

`phase` defaults to `exact_only`. Opt-in `fuzzy_only` or `exact_then_fuzzy`
enables substring edit-distance matching, using the shared matcher and a
threshold of one edit for queries of at most five normalized UTF-8 bytes,
otherwise two edits. Exact results precede fuzzy results; duplicates are removed.
Candidate retrieval uses disjoint query pieces and the incremental postings,
followed by distance verification. Queries are limited to 1024 UTF-8 bytes.
The result limit is applied inside candidate selection before copying all
matching paths. Exact results retain lexical path ordering; fuzzy results
retain distance/path ordering, scope boundaries and exact-result exclusions.

Subscribe on `events.sock`:

```json
{"command":"subscribe_changes","version":1,"scope_path":"/home","actions":["create","delete","modify","rename"],"include_files":true,"include_directories":true}
```

Scope is optional and uses directory boundaries, not raw string prefixes.
Rename matches either its old or new path. Omitted or empty `actions` means
all actions. Subscribers receive `subscribed`, then individual `change` frames.
The `since` object (`epoch`, `sequence`) resumes retained process-local history.
Independent subscribers do not consume one another's events. Expired, future,
or previous-process cursors receive `resync_required` and the connection closes;
clients must refresh their state before subscribing again. Filtering still
advances the server cursor over excluded events.

### Consistent paged refresh

Open a snapshot on `search.sock` after resync (or on initial connection):

```json
{"command":"snapshot","version":1,"page_size":256,"scope_path":"/home","include_files":true,"include_directories":true}
```

Each `snapshot_page` response contains `ok`, `version`, zero-based `page_index`,
`position`, `count`, `entries`, and `done`. Entries use the same IDs and exact
byte paths as search results, plus `is_symlink`. Pages are in raw-byte path
order. While `done=false`, send this request on the **same connection**:

```json
{"command":"snapshot_next","page_size":256}
```

Page size is optional (defaults to 256), must be an integer from 1 to 1024,
and can change between pages. Scope and type filters cannot change. The raw-byte
scope encoding is also supported. Response size is budgeted, so a page can
contain fewer entries than requested even when `done=false`.

All pages retain one SQLite read transaction and the same event position,
captured together under the catalog lock. Concurrent writes do not change
later pages. After receiving `done=true`, subscribe on `events.sock` with that
position as `since` and the same scope/type filters. Apply the returned changes
to the snapshot. If the cursor has expired, restart the refresh instead of
assuming a gap-free handoff. Directory rename/delete events apply to the whole
path prefix; directory create/move-in may contain an existing subtree, for
which the client must refresh a scoped snapshot and replay changes from its
position (while retaining/replaying unrelated changes independently).

At most four snapshots are active; excess requests return `snapshot_busy`.
Sessions have a 30-second lifetime and the normal two-second idle/IO timeout,
then release their read transaction. Closing the connection also releases it.
Snapshots keep WAL history pinned only for their session and do not copy the
entire catalog into service memory. An obsolete epoch, expired event history,
or reconciliation aborts an active snapshot with `resync_required`; discard
every page already received. `snapshot_expired` likewise requires restarting.
When the catalog is not ready, opening a snapshot returns `not_ready`.

Search results and change events carry a directory-entry `id`. Hardlinks have
separate entry IDs; an observed rename preserves the ID. IDs are allocated in
durably reserved blocks and may contain gaps. A filesystem handle including
its filesystem identity is persisted to avoid preserving IDs merely because an
inode number was reused. Unavailable handles cause conservative reassignment
during reconciliation. Unobserved offline renames are not guaranteed to retain
entry IDs.

Linux names need not be UTF-8. `name` and `path` are display strings with invalid
bytes replaced. `name_bytes_hex` and `path_bytes_hex` preserve exact bytes;
rename also includes corresponding `old_*` fields. Use the byte fields when
performing filesystem operations, since display strings can be ambiguous.
Search and subscription requests can use `scope_path_bytes_hex` instead of
`scope_path` to address a non-UTF-8 directory. The two encodings are mutually
exclusive; decoded scopes must be absolute, NUL-free and at most 4096 bytes.

## Durability and recovery boundaries

Bind-mount aliases are indexed as distinct visible paths with distinct entry
IDs. Directory handles retain all visible paths; changes are projected to each
view. Rename endpoints are paired within the same visible mount. A view seeing
only one endpoint receives create/delete rather than a fabricated cross-alias
rename. Directory moves preserve and update descendant paths in each view.

The catalog uses SQLite WAL with `synchronous=FULL`. Snapshot replacement and
incremental changes are transactional; mutations commit before publishing
corresponding events. On startup a completed catalog bound to the same configured
root is loaded into the search index before accepting connections. Cached search
is immediately available after loading; a collector thread in the same process
registers fanotify and enumerates the filesystem in the background to reconcile
downtime changes. Search, status and IPC acceptance do not wait for that scan.
Cache loading/index construction still take time proportional to catalog size;
this is not a constant-time or scan-free validated startup.

Status retains `ready` as fully validated readiness and adds `search_ready` and
`validating`. While startup cache is searchable, `ready=false`, `search_ready=true`
and `validating=true`. Search responses carry `stale=true` until the replacement
index is published; cached paths may have been deleted or renamed offline.
The CLI warns on stderr. After validation, responses carry `stale=false`.
Snapshots and cursor resumption remain unavailable until `ready=true`; they never
advertise stale data as an authoritative snapshot/change boundary. Reconciliation
continues to invalidate old event epochs. Later runtime recovery retains strict
`not_ready` search behavior rather than enabling cached startup again.

The root binding is committed atomically with a completed scan. Legacy databases
without a binding, changed roots and quarantined corrupt databases do not authorize
cached search; they need one successful scan. A validated empty catalog is reusable.
Cached paths are filtered against current filesystem/exclusion policy before serving.

Metadata and object generation are captured from the same no-follow opened
object. A directory disappearing, changing type, or moving during traversal is
treated as a retryable scan race, not an I/O success or a fatal service error.
The service invalidates the old event epoch, retains the previous durable
catalog until a complete replacement can commit, and retries after 100 ms.
Each full reconciliation registers a fresh fanotify queue before enumeration;
records predating that scan are not replayed into its replacement snapshot.
Status reports `ready=false` and a `scan_retries` count during this recovery.
Search returns `ok=false, code="not_ready"` unless a compatible startup cache is
being served; subscriptions require resync.
Permission and genuine I/O errors remain errors rather than being silently
ignored. A stale notification whose object generation differs from the current
indexed/path object also requests reconciliation instead of mutating another
generation. Sustained structural churn can postpone readiness until a scan
completes; the IPC service stays responsive during retry intervals.

Confirmed SQLite corruption (`SQLITE_CORRUPT`, `SQLITE_NOTADB`, or a failed
integrity check) quarantines the service-owned database and existing WAL/SHM
files into a private `catalog.db.corrupt-*` directory before rebuilding from
the configured filesystem root. Status exposes that directory through
`recovered_database`; original files remain available for diagnosis. Permission,
locking, disk-space and I/O failures do not trigger automatic quarantine.
After database loss, entry IDs may be reassigned or reused: clients must discard
their old view and IDs on the new stream epoch, not compare IDs across recovery.

Target file handles also retain identities for short-lived entries whose paths
already vanished when their queued notifications are processed. Such entries
are not inserted into the searchable catalog. Fanotify can coalesce masks for
the same entry: combined create/delete records produce logical create/delete
transitions, but the original syscall ordering relative to a parent rename
cannot always be reconstructed. A child's delete display path may therefore
use the parent name at the coalesced record, rather than its final name. This is
a change-notification stream, not a complete filesystem syscall audit log.
Transient identities and deleted-directory handles are bounded; reaching the
bound triggers reconciliation and subscription resynchronization.
Fanotify has no persistent journal that can replay arbitrary process downtime.
The configured root's kernel object generation is checked at the one-second
mount-check interval because moving the root is reported through its parent,
which may be outside the indexed tree. A changed or missing root invalidates
the stream epoch and makes search/snapshots unavailable. The service retries
while the root is absent, a symlink or a non-directory, then scans a fresh root
and installs a new fanotify group. The last durable catalog is retained but is
not served during recovery. Detection can lag by up to the check interval;
this is not an instantaneous root-lifetime audit. Permission and genuine I/O
failures still fail rather than being treated as a missing directory.
Kernel queue overflow currently reconciles the entire configured root and
invalidates the process-local event history. Subscriber history expiration alone
does not require the server to rescan; that subscriber must refresh its view.

IPC tests exercise byte-fragmented frames, invalid/truncated/oversized requests,
slow-drip bodies, the 32-client cap and shutdown with incomplete clients. A
whole-transfer two-second deadline prevents a sender from retaining a worker
indefinitely by periodically sending another byte. Fanotify uses the same pure
decoder for real reads and malformed-input tests: truncated/unaligned records,
invalid lengths/names, duplicate fields and deterministic mutation/random
sweeps are checked under ASan/UBSan without descriptor side effects or partial
batch commits.

The complete service has been tested with one million real files on WSL ext4;
see [service measurements](linux-index-benchmark.md). The current Ubuntu 24.04
userspace suite passed 30/30 with no skips, including real XFS/Btrfs operations.
Common client tests explicitly cover exact/fuzzy/combined phases, omitted-phase
defaults, invalid phases, and fuzzy results after creation/rename/deletion on
both platforms. This additive API does not replace the existing Windows addon.
The Ubuntu 24.04 chroot test helper runs on the WSL kernel; it verifies userspace
compatibility, not a native Ubuntu kernel or booted systemd deployment.

Additional opt-in tests (not part of the isolated-root default suite):

```sh
sudo python3 tests/linux_full_root_e2e.py /absolute/path/to/FileSearchServiceLinux
sudo python3 tests/linux_systemd_e2e.py /absolute/path/to/FileSearchServiceLinux
```

The full-root test searches `/`, verifies live rename/delete and a scoped
snapshot, and inspects every durable catalog path for host-drive/pseudo/state
exclusions. On the tested WSL instance it passed with 81,423 entries in 2,007 ms
(a smoke measurement, not a performance guarantee). The systemd test uses a
unique transient unit and state directory, mirroring the shipped unit settings;
it does not install or modify the production unit. Live WSL systemd validation
passed root-scope startup, SIGKILL/automatic restart, stable surviving IDs,
offline changes, socket permissions, graceful stop and durable-state retention.
Test-owned state is removed afterward. This is live WSL systemd coverage,
not a native Ubuntu 24.04 boot test.

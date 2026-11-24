# Linux directory sizes

The Linux catalog now persists regular-file logical size, per-path allocated size
(`st_blocks * 512`), modification time and metadata sample time. SQLite migrations
are additive. Legacy entries have `metadata_known=false`; they are never treated
as known zero-byte files. Search/snapshot entries expose the new metadata fields.

`LinuxDirectorySizes` keeps live aggregates in memory. A separate WAL database,
`directory-sizes.db`, persists changed directory totals and is independently
readable while the file catalog opens or the name index rebuilds. The first
upgrade reconstructs totals in 1024-entry catalog pages before name indexing. Updates replace the old contribution and
adjust ancestors. Directory moves remove the old subtree contributions before
adding the new paths. Hardlink aliases share refreshed inode metadata; each
pathname contributes separately to logical and allocated totals. Allocated totals
are **not** deduplicated physical disk usage. Symlinks and special files do not
contribute; the existing ext4/XFS/Btrfs mount and exclusion policy still applies.
"Complete" refers to known metadata within that indexed scope, excluding the
service state directory and unsupported/pseudo filesystems.

## Query protocol

Send a normal framed JSON request to `search.sock`:

```json
{"command":"directory_sizes","paths":["/home","/home/user/project"]}
```

At most 128 absolute paths are accepted. The response is:

```json
{"ok":true,"entries":[{"path":"/home","state":"ready","complete":true,"logical_bytes":4096,"allocated_bytes":8192,"files":2,"unknown":0,"updated_at_ms":1790000000000}]}
```

States are `ready`, `calculating`, `updating`, `stale`, `partial`, `excluded`, and
`unavailable`. Unknown results use `logical_bytes:null`. A known empty directory
has `logical_bytes:0`, `complete:true`. Cold scans publish provisional totals every 128 observations as `partial`,
with `complete:false`, including zero. The full baseline becomes complete after
a valid enumeration. Cached sizes remain available as `stale` during
validation/recovery. The size endpoint is an eventually consistent, sampled
metadata view; it does not extend the namespace snapshot/cursor contract to
atomic file-content snapshots. `updated_at_ms` is the most recent observed sample
or size update, not a claim that every descendant was checked at that instant.

## Background work

The service defaults to a 5ms pause every 512 enumerated entries. Environment:

- `FILESEARCH_SCAN_BATCH`: batch size, 0 disables pauses, maximum 65536.
- `FILESEARCH_SCAN_PAUSE_MS`: pause per batch, maximum 1000.
- `FILESEARCH_RECONCILE_SECONDS`: full reconciliation interval, default 21600;
  0 disables periodic full reconciliation. Overflow/topology recovery remains.

`FAN_MODIFY`, close-write and attribute notifications mark metadata dirty. Changes
are coalesced into bounded batches every 500ms. A bounded round-robin sampler also
checks catalogued files; recently queried directory scopes get priority for 20s.
This updates long-open files and repairs metadata without recursively scanning on
HTTP reads. SQL commits occur only when size/type/mtime/known state changes.
Shutdown cancels enumeration between entries. Large namespace changes and event
queue overflow retain the catalog's reconciliation behavior.

## Verification

The new nonprivileged tests cover aggregate replacement/moves, empty and unknown
sizes, per-path hardlinks, schema migration, durable metadata, sparse files,
symlink exclusion, scan cancellation, and cached/cold query states. Set
`FILESEARCH_TEST_ROOT` to a writable project-local fixture directory.

The real fanotify integration requires root and tests only an isolated subtree:

```sh
sudo python3 FileSearch/tests/linux_directory_sizes_e2e.py \
  bin/ownpanel-filesearch --base-dir /home/mirabox/OwnPanel/data/tmp
```

It covers writes before close, hardlink mutation, directory moves, atomic replace,
delete, restart/offline mutation and exclusions. It returns 77 without root; that
is a skip, not a successful live validation. Existing legacy tests with hardcoded
`/home/donggua` fixtures are not invoked by the OwnPanel build script.

## Startup search fallback

Listeners are opened before `LinuxCatalog::Start` runs on its collector thread.
Status responses expose `initializing:true` during this stage. Size reads use
independent SQLite connections and never wait on the full file/name index.
Cache replacement is transactional, so readers retain the preceding generation
until commit. Existing search/snapshot/change contracts remain; snapshots and
change subscription readiness still require namespace reconciliation.

`search` requests work during initialization using a read-only cached-name query
with a 500ms VM deadline. Responses carry `stale:true`, `initializing:true` and
`partial:true` if interrupted. Literal substring matches respect path-component
scope boundaries. Root binding and mount/state exclusions are checked before
returning results. Add `compact:true` to omit byte-hex fields in search responses.
The normal in-memory name query takes over when usable.

`linux_query_cache` covers persistence, incomplete/deleted directories, concurrent
reads, Chinese/literal filename queries and scope isolation. The nonprivileged
`linux_query_startup_test.py` holds an exclusive lock on a catalog and verifies
that sockets, cached sizes and bounded search still respond before it unlocks.

## Scoped repair and differential reconciliation

Queued notification generations may predate a newly enumerated baseline. A
matching current entry avoids replaying the create; other generation mismatches
repair only that path. Missing/replaced files update SQLite, aliases, size
ancestors and name postings together. Directory repairs scan their own subtree
without following symlink components. Local scan races retry that scope with
250ms–32s backoff while other scopes remain queryable. Unrelated entry IDs and
the change epoch are preserved. A successful global reconciliation still fences
snapshot cursors by resetting its epoch.

With an independent query cache configured, full reconciliation applies a sorted
snapshot difference in place. Existing name postings survive unchanged paths;
only namespace additions/deletions/name changes update them. Stable size facts
avoid ancestor subtraction/addition. New and old full name indexes no longer
coexist, and scan storage is released before size-cache publication. Bounded SQL
queries and independent directory-cache reads cover the merge interval.

Large dirty metadata sets select 512 entries per sampling cycle rather than
escalating solely because size facts are dirty. True fanotify overflow, root or
mount changes and excessive unresolved namespace history retain global recovery.
The local-repair test verifies these paths without requiring fanotify privileges;
real queue/event integration remains a separately gated root test.

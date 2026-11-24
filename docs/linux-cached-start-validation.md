# Linux cached startup validation (2026-10-13)

The service now loads a compatible persisted catalog, builds its search index,
accepts IPC and scans in a collector thread in the same process. Windows production
implementation, service binaries and cache formats are unchanged.

## Contract

- `ready`: full catalog validation completed; existing snapshot/replay semantics.
- `search_ready`: either validated search or compatible startup cache is usable.
- `validating`: catalog reconciliation is still in progress.
- Search `stale`: sampled with the result under the search mutex, true for startup
  cached results. The CLI prints a warning unless structured JSON was requested.
- Snapshot creation is refused with `not_ready` before validation. Subscriptions
  require resynchronization. Offline changes are not invented as historical events.
- SQLite records the search root atomically with completed catalog replacement;
  legacy/unbound/corrupt/different-root caches need an initial scan. Empty validated
  catalogs are reusable. Current mount and exclusion policy filters cached paths.
- Database replacement, new-index construction and old-index destruction do not
  hold the search mutex. Publishing the new index does.

## Coverage

`linux_cached_start_test` checks a fresh store, reusable cached pinyin/IDs, offline
delete/create correction, unavailable unvalidated snapshots/cursors, changed roots
and reusable empty catalogs. `linux_store_test` checks binding transaction rollback.
`node_cached_start_e2e.js` runs the actual service twice over 30,001 files, observes
cached search while validation is active, rejects unvalidated snapshots and verifies
the corrected results. Existing IPC tests now explicitly wait for `ready` when they
need validated results; socket existence alone no longer guarantees readiness.

WSL Ubuntu: full CTest run had no failures (32 passes and one Btrfs skip), followed
by a passing Btrfs image test after loading the module. Concurrent overflow/recovery
subsequently passed 15 consecutive repetitions. An earlier stress run reported one
missing durable path; it did not reproduce in these repetitions and is not claimed
as a diagnosed/fixed issue.

Remote `dongguabao.cn`, Ubuntu 24.04.4, kernel 6.8.0-63, Node 24.15.0:
native build, cached-start unit test, SQLite test, cached-start real service test
and existing search/change-stream Node integration test passed. One synthetic
30,001-file run completed cached search plus early IPC checks in 794 ms and full
validation in 2,587 ms. These are single-run timings, not general disk benchmarks;
cache loading still scales with index size and validation still enumerates directories.

Remote artifacts remain in `/home/donggua/filesearch-remote-cli-8WQ6CI`.
Test fixture services stop and fixture directories are removed. The SSH connection
is retained; no system-wide deployment is performed.

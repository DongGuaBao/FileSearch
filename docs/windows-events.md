# Windows change subscriptions

The existing search pipe, MFT enumeration and cache formats are unchanged.
Changes use a separate `\\.\pipe\FileSearch.Events` endpoint with 32-bit
little-endian length-prefixed UTF-8 JSON frames (maximum 1 MiB).

```json
{"command":"subscribe_changes","version":1,"scope_path":"D:\\Work","actions":["rename","delete"],"include_files":true,"include_directories":false}
```

`scope_path` is optional, case-insensitive, and matches directory boundaries.
Rename matches either endpoint. `actions` can contain `create`, `delete`,
`modify`, and `rename`; omitted or empty means all actions. Unknown actions
are rejected. Omitted type filters both default to true. Both false yields no
changes. Filters do not prevent the reader cursor advancing over other events.

The first response is `subscribed`. Subsequent `change` frames include action,
directory-entry identity, paths, directory flag and an epoch/sequence position.
Clients can supply `since: {"epoch": ..., "sequence": ...}` on reconnect.
Readers have independent cursors and do not drain one another's events.
An expired, future or prior-process cursor receives `resync_required` and closes.
History is process-local, not a durable cross-restart event journal. Native USN
updates drive the stream; it is not a complete syscall audit log.

Windows IDs are 64-bit FRNs; consumers must parse integer identities without
loss of precision (ordinary JavaScript numbers are not sufficient).
The additive [Node service client](../clients/node/README.md) handles this and
preserves the existing message-mode header/body write boundaries. Its search,
subscriptions and exact cursor resume are covered by the Windows e2e test.
The original search pipe additionally honors an explicitly supplied `phase`
(`exact_only`, `fuzzy_only`, `exact_then_fuzzy`) using the additive native
`SearchWithPhase` entry point. Ngram mode supports fuzzy phases; Filter mode
returns an error for an unsupported fuzzy request. When phase is omitted, the
original `Search` entry point remains in use, including its disabled fuzzy
fallback. The native addon's original call path and options layout are unchanged.
The common Node client supplies `exact_only` explicitly unless the caller
chooses a different phase. Existing search-engine algorithms are reused as-is.
Explicit fuzzy phases also check retained WAL names using the existing fuzzy
matcher and validate their current kernel paths, so newly created files need
not wait for the base ngram index rebuild. The legacy entry point is unchanged.

## Consistent snapshot and cursor

`AutoFileSearch::CaptureChangeSnapshot` exports an owned MFT snapshot and its
event position under a batch fence. The export merges retained WAL changes
without modifying the live search index or rewriting the existing cache. It
also works after `ReleaseFacts`, using the persisted base plus the WAL. An
append failure, journal replacement/gap, or unexpected journal read failure
invalidates the stream epoch and disables capture and event replay until
restart/rebuild (including when the read failure later clears);
callers must check the returned boolean and must not publish a failed snapshot.
Names retain the existing lowercased MFT representation.

The same events pipe accepts `{"command":"snapshot","version":1,"page_size":256}`.
Optional `scope_path`, `include_files` and `include_directories` filters are fixed
for that connection. `page_size` must be 1..1024. Each `snapshot_page` includes
`entries`, `count`, `page_index`, `done`, and the same captured `position`.
Entries include `id`, `parent_id`, `name`, `path` and `is_directory`. Send
`{"command":"snapshot_next"}` on the same connection for each next page;
only `page_size` can change between pages. After `done`, subscribe on a new
connection with `since` set to that position and matching scope/type filters.
Discard partial pages on any error or `resync_required` response.

Pages stay below a conservative 512 KiB entry budget and 1 MiB frame limit.
There is one active owned capture, a two-second idle/I/O limit and a 120-second
post-capture session limit; a competing capture returns `snapshot_busy`.
Snapshot copies and quota are released before delivery of `done`. Final frames
remain available until peer closure or a bounded two-second drain; the server
does not use an unbounded synchronous pipe flush. Ring expiration, epoch changes
and service restart require resynchronization. Large catalogs should use
`page_size:1024` and a client timeout allowing the initial capture.

Fixed NTFS metadata ancestors omitted by USN enumeration (including volume
roots) are verified by full kernel file ID and filled only into the export.
A missing ordinary directory, mismatched generation, cyclic ancestry or invalid
path fails the snapshot instead of fabricating a path. The existing search pipe
protocol and cache/WAL formats remain unchanged.

The opt-in `windows_snapshot_live_test` CMake target links the freshly rebuilt
Release DLL and requests administrator elevation. Run it with two absolute
arguments: a fresh test-owned directory on the NTFS drive to enumerate, and a
result text file. It uses an isolated cache, checks real create/rename/delete
notifications, verifies their snapshot membership and cursor, and covers the
released-facts path. The fixture/cache directory is retained for inspection.

`windows_snapshot_e2e.js` checks real-service snapshot quotas, idle cleanup,
type filters and invalid requests; `--full` consumes the full exported catalog
and reports page count and elapsed time. `node_service_client_e2e.js` verifies
immutable pages under real NTFS writes and snapshot-plus-event reconstruction.
`windows_snapshot_cache_probe` is an opt-in read-only diagnostic target for
examining parent completeness of an existing MFT snapshot file.

The additive events endpoint permits at most 32 concurrent workers (including
subscriptions and snapshots); excess connections are closed. Its request and
write transfers have two-second deadlines. Idle disconnected subscribers are
reclaimed within the 500 ms event wait interval. Final error/resync frames have
a bounded two-second delivery grace instead of an unbounded pipe flush. The
legacy search endpoint's limits and timeout behavior are unchanged.
`windows_ipc_limits_e2e.js --shutdown` tests malformed/incomplete requests,
capacity rejection, slot reuse, repeated disconnects, legacy endpoint readiness
under saturation, and stop with open subscriptions/incomplete requests.
`windows_change_health_test` uses an unconfigured service with no volume/cache
IO to inject stream invalidation and a closed-WAL append failure through the
production helpers: old and fresh cursors expire, snapshots fail, waiters wake,
and repeated failures do not continuously replace the epoch. It does not test
physical disk failure or alter a real USN journal.

# Windows full-catalog snapshot validation

Local Release service, existing production-shaped MFT/search caches, October 2,
2026. Three NTFS volumes (C, D, E); this is a real service/IPC/client export, not
an index-only synthetic benchmark. Command: `node tests/windows_snapshot_e2e.js
--full` against an already-running elevated service.

The verified run exported 4,520,624 entries in 4,415 pages (`page_size:1024`)
in 54,418 ms. It included all three volume-root rows. The Node client decoded
and checked 64-bit entry IDs and absolute Windows paths without accumulating
the full result. Every page carried the same capture epoch/sequence. No memory
or cold-start claim is derived from this run.

The existing Windows service e2e additionally validates scoped immutable pages
while real files are created, renamed and deleted, then reconstructs the actual
directory using events after the snapshot cursor. The separate snapshot-limits
test covers the one-capture quota, release after idle/disconnect/completion,
empty type selection, and invalid scope/filter/page-size requests.

The full export exceeded 30 seconds, so Windows uses a 120-second post-capture
session limit (Linux's SQLite snapshot retains its 30-second limit). Both use
bounded two-second snapshot I/O/idle waits. Counts and timing are from a warm
local run and are not a guaranteed upper bound on other machines or catalogs.

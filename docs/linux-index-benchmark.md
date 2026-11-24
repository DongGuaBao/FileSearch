# Linux name-index benchmark

Measured on local WSL ext4, Release build, 2026-10-02. Run:

```sh
build/linux_index_benchmark 100000
build/linux_index_benchmark 1000000
```

The fixture inserts deterministic `report_N.txt` names beneath a long common
path, runs 100 identical selective queries, materializes one `.txt` query,
then deletes 10% of the entries and verifies deletion. Peak RSS includes result
materialization and deletion. It exercises the in-memory name index only, not
filesystem scanning, SQLite, IPC, the catalog's additional maps, or subscriptions.

| Index representation | Entries | Build | 100 selective queries | Peak RSS |
| --- | ---: | ---: | ---: | ---: |
| Path strings in tree postings (baseline) | 100,000 | 3,342 ms | 5,291 ms | 679,496 KiB |
| Numeric IDs in tree postings | 100,000 | 2,037 ms | <1 ms | 246,092 KiB |
| Numeric IDs in sparse 64-ID bitmap blocks | 100,000 | 737 ms | 484 µs | 91,696 KiB |
| Numeric IDs in sparse 64-ID bitmap blocks | 1,000,000 | 9,303 ms | 9,922 µs | 888,052 KiB |

The final bitmap version materialized all matches for `.txt` in 61 ms at
100,000 entries and 752 ms at 1,000,000 entries. Earlier baselines did not include
that extra broad-query measurement, so their RSS is not an identical-workload
comparison. Selective queries are warm and synthetic; these results do not prove
production latency, worst-case fuzzy performance, or complete service memory use.

Numeric posting IDs are internal to the name index, not the persisted public
directory-entry IDs. Deletion removes the bit and reclaims empty blocks; updates
do not accumulate tombstones. Query verification still checks full normalized
terms after gram filtering. Exact and fuzzy tests compare expected membership,
and fuzzy recall is also compared against exhaustive edit-distance matching.

## Complete service fixture

```sh
sudo python3 tests/linux_service_scale.py build/FileSearchServiceLinux 100000
```

This fixture creates 100,000 real empty files in 100 directories on WSL ext4,
starts the service, checks the durable entry count, runs 100 selective socket
queries and one common-extension query, then validates 50 create/rename/delete
cycles through sockets. Event IDs and searches are checked at every step. The
database integrity and final count are verified before normal shutdown.

After replacing full-map mutations with path/subtree lookups and using fanotify
descriptor plus event-ring wakeups, the measured result was:

- startup including enumeration, indexing and snapshot commit: 2,495 ms;
- service peak RSS: 151,976 KiB;
- selective search median / p95: 0.187 / 0.286 ms;
- `.txt` search with 50-result limit: 92.42 ms;
- 50 verified create/rename/delete cycles: 541.52 ms total.

The same change-cycle fixture took 10,047 ms before either optimization and
7,539 ms with only path/subtree lookups. These are local warm synthetic runs,
not portable service-level guarantees. The 10,000-file version is registered
in CTest as `linux_service_scale`; larger runs are explicit benchmarks.

## Million-file complete service run (October 2, 2026)

The same complete-service fixture was run with 1,000,000 real files and 1,000
directories on WSL ext4. Enumeration, kernel-generation capture, indexing,
SQLite snapshot commit, socket search, 50 real mutation/push cycles, final
durable count and SQLite integrity were all exercised.

| Complete-service measurement | Before limited candidate results | After |
| --- | ---: | ---: |
| Startup | 20,654 ms | 20,622 ms |
| Peak service RSS | 1,412,804 KiB | 1,318,872 KiB |
| Selective search median / p95 | 0.457 / 0.722 ms | 0.459 / 0.641 ms |
| `.txt`, max 50 results | 751.18 ms | 97.93 ms |
| 50 verified create/rename/delete cycles | 459.53 ms | 471.93 ms |

Limited exact search visits matching posting IDs without first materializing
all matching IDs and paths. It retains only the lexically first requested
results within the scope. Limited fuzzy search preserves score/path ordering
and exclusions while retaining only the requested result paths; its internal
candidate-ID vector can still grow with query breadth. Exhaustive differential
tests verify scope boundaries, exact-file scopes, limits, fuzzy scores/order
and exclusions, including short-query fallback. Broad matching still visits
candidates and is not O(result limit). These warm synthetic timings are not
portable latency guarantees or a million-file fuzzy-query SLA.

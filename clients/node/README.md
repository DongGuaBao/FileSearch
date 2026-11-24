# FileSearch service client

## 命令行搜索（Windows / Linux）

需要 Node.js 18+，并先启动对应平台的 FileSearch 服务。在仓库根目录执行：

```powershell
# Windows
node clients/node/cli.js "报告"
node clients/node/cli.js --scope "D:\Develop" --limit 20 "FileSearch"
node clients/node/cli.js --status
```

```bash
# Linux：默认服务 socket 仅允许 root 访问
sudo node clients/node/cli.js "报告"
sudo node clients/node/cli.js --scope /home --limit 20 "FileSearch"
# 服务使用自定义状态目录时
sudo node clients/node/cli.js --ipc-dir /path/to/state "报告"
```

默认只执行精确阶段（文件名子串/拼音搜索）；`--phase fuzzy` 使用模糊阶段，
`--phase both` 使用精确加模糊阶段。`--json` 输出结构化响应，64 位 ID
以字符串保留精度。普通输出每行一个路径，数量和耗时写入 stderr。
没有匹配项时成功退出；参数错误退出码为 2，服务错误为 1。
`--help` 查看全部选项；`--timeout 60000` 可用于首次索引较慢的情况。
此工具连接现有索引服务，不直接遍历磁盘，也不会自动启动或安装服务。
Linux 在启动校验期间可返回缓存结果，普通输出会在 stderr 提示；JSON 中
`stale: true` 表示结果尚未校验。状态中的 `search_ready` 表示可以搜索，
`ready` 表示校验已完成，`validating` 表示正在校验。

Dependency-free CommonJS client for Node.js 18+. This is an additive service
API; it does not replace the existing Windows native addon or its direct calls.
The collector, index and search service still run in one service process.

```js
const { FileSearchClient } = require('./clients/node');
const client = new FileSearchClient();
const results = await client.search('zhongwen', { max_results: 50 });
const abort = new AbortController();
for await (const event of client.subscribeChanges({ actions: ['create', 'rename', 'delete'] },
    { signal: abort.signal })) {
    if (event.type === 'resync_required') break; // refresh before reconnecting
    // Handle subscribed/change. Do not stringify BigInt with plain JSON.stringify.
}
```

Defaults are Windows named pipes or Linux `/var/lib/filesearch/{search,events}.sock`.
Set `ipcDirectory` for another Linux state directory. Tests can override
`searchEndpoint`/`eventsEndpoint`. `timeoutMs` (default 5000) bounds connection,
request and initial subscription response waits; established subscriptions have
no idle deadline. Abort signals cancel requests/streams. Break/return from an
iterator closes the connection. Reading is demand-driven with socket backpressure,
not an unbounded application event queue. A disconnect throws; this client does
not silently reconnect across gaps.

IDs, FRNs, parent IDs and epoch/sequence fields are **BigInt**, including small
values. Serialization sends exact integers for `since`; no Number conversion is
needed. Use `.toString()` when storing IDs in ordinary JSON. The new client's
Windows search results expose an `id` alias for the original `frn`; native-addon
results remain unchanged. Linux additionally returns exact filename bytes as hex.

`status()` and `request(body)` return response objects. `search(query, options)`
defaults explicitly to `exact_only` on both platforms; fuzzy phases are opt-in.
Server failures throw `FileSearchError` with `code` and `response`. Established
subscriptions yield terminal `resync_required`/`error` frames, then finish.

`snapshot(options)` yields immutable pages without accumulating them on both platforms.
All pages carry the same position; after the final page, subscribe with `since`
and matching filters. Snapshot failures (including resync) throw; discard partial
pages. Both servers enforce a two-second idle limit. Linux permits four active
snapshots with a 30-second session limit; Windows permits one owned capture with
a 120-second session limit because existing NTFS catalogs can contain millions
of entries. Use `page_size: 1024` for a full-disk export and configure a longer
client `timeoutMs` (for example 60000) to allow the initial capture. Windows
snapshots use the additive events pipe, not the original search pipe. Names and
path components retain the deployed Windows index's lowercased representation;
compare Windows paths case-insensitively. The existing native addon is unchanged.

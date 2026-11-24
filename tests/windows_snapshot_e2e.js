'use strict';
// Requires an already-running elevated production service. Never changes its
// configuration or cache. Fixture mutation uses a fresh, owned child directory.
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const { FileSearchClient } = require('../clients/node');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function run() {
    const client = new FileSearchClient({ timeoutMs: 60000 });
    const transport = { signal: AbortSignal.timeout(180000) };
    let fixture;
    let held;
    try {
        const deadline = Date.now() + 120000;
        while (!(await client.status(transport)).ready) {
            assert.ok(Date.now() < deadline, 'service not ready');
            await sleep(100);
        }
        if (process.argv.includes('--full')) {
            const start = Date.now();
            let count = 0, pages = 0, position;
            const roots = new Set();
            for await (const page of client.snapshot({ page_size: 1024 }, transport)) {
                ++pages;
                count += page.entries.length;
                position = page.position;
                for (const entry of page.entries) {
                    assert.equal(typeof entry.id, 'bigint');
                    assert.match(entry.path, /^[A-Z]:\\/);
                    if (entry.parent_id === 0n) roots.add(entry.path);
                }
            }
            assert.ok(count > 0 && roots.size > 0);
            console.log(JSON.stringify({ test: 'windows_full_snapshot', count, pages,
                milliseconds: Date.now() - start, roots: [...roots],
                epoch: position.epoch.toString(), sequence: position.sequence.toString() }));
            return;
        }
        fixture = await fs.mkdtemp(path.join(__dirname, 'snapshot-limits-'));
        for (const name of ['a.txt', 'b.txt', 'c.txt']) await fs.writeFile(path.join(fixture, name), 'fixture');
        const live = client.subscribeChanges({ scope_path: fixture, actions: ['create'], include_directories: false }, transport);
        await live.next();
        await fs.writeFile(path.join(fixture, 'ready.txt'), 'fence');
        await live.next();
        await live.return();
        held = client.snapshot({ scope_path: fixture, page_size: 1 }, transport);
        const first = (await held.next()).value;
        assert.equal(first.type, 'snapshot_page'); assert.equal(first.done, false);
        await assert.rejects(async () => {
            for await (const unused of client.snapshot({ scope_path: fixture }, transport)) assert.fail('quota bypass');
        }, error => error.code === 'snapshot_busy');
        await sleep(2500);
        await assert.rejects(() => held.next(), error => ['connection_closed', 'EPIPE', 'ECONNRESET'].includes(error.code));
        await held.return(); held = undefined;
        const entries = [];
        for await (const page of client.snapshot({ scope_path: fixture, include_directories: false }, transport))
            entries.push(...page.entries);
        assert.equal(entries.length, 4, 'idle connection retained snapshot quota');
        const none = [];
        for await (const page of client.snapshot({ scope_path: fixture, include_directories: false,
            include_files: false }, transport)) none.push(...page.entries);
        assert.equal(none.length, 0);
        for (const options of [{ page_size: 0 }, { page_size: 1025 }, { actions: ['create'] }, { scope_path: 'relative' }])
            await assert.rejects(async () => {
                for await (const unused of client.snapshot(options, transport)) assert.fail('invalid options accepted');
            }, error => error.code === 'snapshot_failed');
        console.log('windows_snapshot_limits passed');
    } finally {
        if (held) await held.return();
        if (fixture) await fs.rm(fixture, { recursive: true, force: true });
    }
}
run().catch(error => { console.error(error); process.exitCode = 1; });

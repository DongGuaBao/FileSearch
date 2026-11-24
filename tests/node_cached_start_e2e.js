'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { FileSearchClient } = require('../clients/node');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function main() {
    if (process.platform !== 'linux' || process.getuid() !== 0) { process.exitCode = 77; return; }
    const base = fs.mkdtempSync('/home/donggua/filesearch-cached-e2e-');
    const root = path.join(base, 'files'), ipc = path.join(base, 'ipc');
    fs.mkdirSync(root); fs.mkdirSync(ipc);
    let service, finished, log = '';
    const start = () => {
        service = spawn(process.argv[2], [root, ipc], { stdio: ['ignore', 'ignore', 'pipe'] });
        service.stderr.on('data', bytes => { log += bytes; });
        finished = new Promise(resolve => service.once('exit', resolve));
    };
    const stop = async () => { service.kill('SIGTERM'); await finished; assert.equal(service.exitCode, 0, log); };
    const client = new FileSearchClient({ ipcDirectory: ipc, timeoutMs: 10000 });
    const status = async () => {
        const deadline = Date.now() + 30000;
        while (Date.now() < deadline) {
            assert.equal(service.exitCode, null, log);
            try { return await client.status(); }
            catch (error) { if (!['ENOENT', 'ECONNREFUSED'].includes(error.code)) throw error; }
            await sleep(5);
        }
        throw new Error('status timeout: ' + log);
    };
    const ready = async () => {
        const deadline = Date.now() + 30000;
        while (!(await status()).ready) { assert.ok(Date.now() < deadline, 'validation timeout'); await sleep(5); }
    };
    try {
        fs.writeFileSync(path.join(root, 'cached_unique.txt'), 'offline delete');
        // Keep actual enumeration long enough to observe background service IPC.
        for (let i = 0; i < 30000; ++i) fs.writeFileSync(path.join(root, `padding_${i}.txt`), '');
        start(); await ready();
        const baseline = await client.search('cached_unique');
        assert.equal(baseline.stale, false); assert.equal(baseline.results.length, 1);
        await stop();
        fs.unlinkSync(path.join(root, 'cached_unique.txt'));
        fs.writeFileSync(path.join(root, 'offline_unique.txt'), 'offline create');
        const started = Date.now();
        start();
        const early = await status();
        assert.equal(early.search_ready, true);
        assert.equal(early.ready, false, 'fixture scan finished before stale-cache observation');
        assert.equal(early.validating, true);
        const cached = await client.search('cached_unique');
        assert.equal(cached.stale, true); assert.equal(cached.results[0].id, baseline.results[0].id);
        assert.equal((await client.search('offline_unique')).results.length, 0);
        await assert.rejects(client.snapshot({}).next(), error => error.code === 'not_ready');
        const searchableMs = Date.now() - started;
        await ready();
        assert.equal((await client.search('cached_unique')).results.length, 0);
        const corrected = await client.search('offline_unique');
        assert.equal(corrected.stale, false); assert.equal(corrected.results.length, 1);
        console.log(JSON.stringify({ test: 'cached startup real service passed', files: 30001,
            cache_search_ms: searchableMs, validated_ms: Date.now() - started }));
        await stop();
    } finally {
        if (service && service.exitCode === null) { service.kill('SIGTERM'); await finished; }
        fs.rmSync(base, { recursive: true, force: true });
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });

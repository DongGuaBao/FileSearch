'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const { FileSearchClient } = require('../clients/node');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function run() {
    const windows = process.argv[2] === '--windows-live';
    let base, root, service, exited, stderr = '';
    let events, replay, snapshotReplay, snapshotRoot;
    let original, renamed, lifecycleRoot;
    const signal = AbortSignal.timeout(45000);
    try {
        let client;
        if (windows) {
            root = process.argv[3];
            assert.ok(root, 'Windows fixture directory required');
            client = new FileSearchClient({ timeoutMs: 60000 });
        } else {
            if (process.getuid() !== 0) { process.exitCode = 77; return; }
            base = await fs.mkdtemp('/home/donggua/filesearch-node-e2e-');
            root = path.join(base, 'files');
            const ipc = path.join(base, 'ipc');
            await fs.mkdir(root); await fs.mkdir(ipc);
            await fs.writeFile(path.join(root, '中文.txt'), 'pinyin');
            await fs.writeFile(Buffer.concat([Buffer.from(root + '/raw_'), Buffer.from([255])]), 'bytes');
            service = spawn(process.argv[2], [root, ipc], { stdio: ['ignore', 'ignore', 'pipe'] });
            service.stderr.on('data', chunk => { stderr += chunk; });
            exited = new Promise(resolve => service.once('exit', code => resolve(code)));
            client = new FileSearchClient({ ipcDirectory: ipc });
        }
        const readyDeadline = Date.now() + 10000;
        while (true) {
            try { if ((await client.status({ signal })).ready) break; }
            catch (error) { if (!['ENOENT', 'ECONNREFUSED'].includes(error.code)) throw error; }
            assert.ok(!service || service.exitCode === null, stderr);
            assert.ok(Date.now() < readyDeadline, 'service readiness timeout');
            await sleep(20);
        }
        if (!windows) {
            assert.equal((await client.search('zhongwen')).results.length, 1);
            const captured = [];
            let position;
            for await (const page of client.snapshot({ page_size: 1 }, { signal })) {
                captured.push(...page.entries); position = page.position;
            }
            assert.equal(captured.length, 2);
            assert.ok(captured.every(entry => typeof entry.id === 'bigint'));
            assert.ok(captured.some(entry => entry.path_bytes_hex.endsWith('ff')));
            assert.equal(typeof position.epoch, 'bigint');
        } else {
            snapshotRoot = await fs.mkdtemp(path.join(root, 'node-snapshot-'));
            for (const name of ['a.txt', 'b.txt', 'c.txt', 'd.txt'])
                await fs.writeFile(path.join(snapshotRoot, name), 'baseline');
            // Wait for USN monitoring, not merely search's independent merge.
            const initialEvents = client.subscribeChanges({ scope_path: snapshotRoot,
                actions: ['create'], include_directories: false }, { signal });
            await initialEvents.next();
            await fs.writeFile(path.join(snapshotRoot, 'ready.txt'), 'fence');
            assert.equal((await initialEvents.next()).value.action, 'create');
            await initialEvents.return();
            const captured = [];
            let position;
            const pages = client.snapshot({ scope_path: snapshotRoot, page_size: 1,
                include_directories: false }, { signal });
            for await (const page of pages) {
                captured.push(...page.entries); position = page.position;
                if (page.page_index === 0) {
                    await fs.rename(path.join(snapshotRoot, 'c.txt'), path.join(snapshotRoot, 'renamed.txt'));
                    await fs.unlink(path.join(snapshotRoot, 'b.txt'));
                    await fs.writeFile(path.join(snapshotRoot, 'new.txt'), 'new');
                }
            }
            assert.deepEqual(captured.map(entry => entry.name).sort(), ['a.txt', 'b.txt', 'c.txt', 'd.txt', 'ready.txt']);
            assert.ok(captured.every(entry => typeof entry.id === 'bigint' && typeof entry.parent_id === 'bigint'));
            const reconstructed = new Map(captured.map(entry => [entry.id, entry.path.toLowerCase()]));
            snapshotReplay = client.subscribeChanges({ since: position, scope_path: snapshotRoot,
                actions: ['create', 'rename', 'delete'], include_directories: false }, { signal });
            assert.equal((await snapshotReplay.next()).value.type, 'subscribed');
            for (let i = 0; i < 3; ++i) {
                const change = (await snapshotReplay.next()).value;
                if (change.action === 'delete') reconstructed.delete(change.id);
                else reconstructed.set(change.id, change.path.toLowerCase());
            }
            const actual = (await fs.readdir(snapshotRoot)).map(name => path.join(snapshotRoot, name).toLowerCase()).sort();
            assert.deepEqual([...reconstructed.values()].sort(), actual, 'snapshot + cursor replay differs from real NTFS');
            await snapshotReplay.return(); snapshotReplay = undefined;
            const scoped = [];
            for await (const page of client.snapshot({ scope_path: path.join(snapshotRoot, 'renamed.txt'),
                include_directories: false }, { signal })) scoped.push(...page.entries);
            assert.equal(scoped.length, 1); assert.equal(scoped[0].name, 'renamed.txt');
        }
        const phaseRoot = windows ? snapshotRoot : root;
        await fs.writeFile(path.join(phaseRoot, 'planets.txt'), 'phase parity');
        const phaseDeadline = Date.now() + 10000;
        while (!(await client.search('planets', { scope_path: phaseRoot }, { signal })).results.length) {
            assert.ok(Date.now() < phaseDeadline, 'phase fixture not searchable');
            await sleep(20);
        }
        assert.equal((await client.search('planetz', { scope_path: phaseRoot, phase: 'exact_only' }, { signal })).results.length, 0);
        let fuzzyPhase;
        do {
            fuzzyPhase = await client.search('planetz', { scope_path: phaseRoot, phase: 'fuzzy_only' }, { signal });
            if (!fuzzyPhase.results.some(entry => entry.name === 'planets.txt')) await sleep(20);
        } while (!fuzzyPhase.results.some(entry => entry.name === 'planets.txt') && Date.now() < phaseDeadline);
        assert.ok(fuzzyPhase.results.some(entry => entry.name === 'planets.txt'), 'explicit fuzzy phase ignored');
        const combinedPhase = await client.search('planetz', { scope_path: phaseRoot, phase: 'exact_then_fuzzy' }, { signal });
        assert.ok(combinedPhase.results.some(entry => entry.name === 'planets.txt'));
        const legacyDefault = await client.request({ query: 'planetz', scope_path: phaseRoot }, { signal });
        assert.equal(legacyDefault.results.some(entry => entry.name === 'planets.txt'), false, 'omitted phase default changed');
        await assert.rejects(() => client.search('planets', { phase: 'invalid' }, { signal }));
        // WAL overlays must validate current names and not resurrect renamed
        // or deleted files from historical matching entries.
        const phaseRenamed = path.join(phaseRoot, 'galaxies.txt');
        await fs.rename(path.join(phaseRoot, 'planets.txt'), phaseRenamed);
        const renamedDeadline = Date.now() + 10000;
        while (!(await client.search('galaxies', { scope_path: phaseRoot }, { signal })).results.length) {
            assert.ok(Date.now() < renamedDeadline, 'renamed phase fixture not searchable');
            await sleep(20);
        }
        assert.equal((await client.search('planetz', { scope_path: phaseRoot, phase: 'fuzzy_only' }, { signal }))
            .results.some(entry => entry.name === 'planets.txt'), false);
        assert.ok((await client.search('galaxiez', { scope_path: phaseRoot, phase: 'fuzzy_only' }, { signal }))
            .results.some(entry => entry.name === 'galaxies.txt'));
        await fs.unlink(phaseRenamed);
        const deletedPhaseDeadline = Date.now() + 10000;
        while ((await client.search('galaxiez', { scope_path: phaseRoot, phase: 'fuzzy_only' }, { signal }))
            .results.some(entry => entry.name === 'galaxies.txt')) {
            assert.ok(Date.now() < deletedPhaseDeadline, 'deleted fuzzy result retained');
            await sleep(20);
        }
        // Isolate this stream from delayed events for earlier phase fixtures.
        lifecycleRoot = path.join(root, 'lifecycle-' + crypto.randomUUID());
        await fs.mkdir(lifecycleRoot);
        events = client.subscribeChanges({ scope_path: lifecycleRoot, actions: ['create', 'rename', 'delete'],
            include_directories: false }, { signal });
        const acknowledged = (await events.next()).value;
        assert.equal(acknowledged.type, 'subscribed');
        assert.equal(typeof acknowledged.position.epoch, 'bigint');
        const token = 'node_client_' + crypto.randomUUID().replaceAll('-', '');
        original = path.join(lifecycleRoot, token + '_original.txt');
        renamed = path.join(lifecycleRoot, token + '_renamed.txt');
        await fs.writeFile(original, 'client');
        const created = (await events.next()).value;
        assert.equal(created.action, 'create'); assert.equal(created.path, original);
        assert.equal(typeof created.id, 'bigint');
        await fs.rename(original, renamed);
        const moved = (await events.next()).value;
        assert.equal(moved.action, 'rename'); assert.equal(moved.old_path, original);
        assert.equal(moved.path, renamed); assert.equal(moved.id, created.id);
        let result;
        const deadline = Date.now() + 15000;
        do {
            result = await client.search(token + '_renamed', { scope_path: root }, { signal });
            if (!result.results.length) await sleep(50);
        } while (!result.results.length && Date.now() < deadline);
        assert.equal(result.results.length, 1); assert.equal(result.results[0].id, created.id);
        await fs.unlink(renamed);
        const deleted = (await events.next()).value;
        assert.equal(deleted.action, 'delete'); assert.equal(deleted.id, created.id);
        replay = client.subscribeChanges({ since: acknowledged.position, scope_path: lifecycleRoot,
            actions: ['create', 'rename', 'delete'], include_directories: false }, { signal });
        assert.equal((await replay.next()).value.type, 'subscribed');
        for (const expected of [created, moved, deleted]) {
            const received = (await replay.next()).value;
            assert.equal(received.action, expected.action);
            assert.deepEqual(received.position, expected.position);
            assert.equal(received.id, expected.id);
        }
        assert.equal((await client.search(token + '_renamed', { scope_path: root })).results.length, 0);
        console.log('node_service_client_e2e passed (' + (windows ? 'Windows' : 'Linux') + ')');
    } finally {
        for (const fixture of [original, renamed]) if (fixture) await fs.rm(fixture, { force: true });
        if (events) await events.return();
        if (replay) await replay.return();
        if (snapshotReplay) await snapshotReplay.return();
        if (snapshotRoot) await fs.rm(snapshotRoot, { recursive: true, force: true });
        if (lifecycleRoot) await fs.rm(lifecycleRoot, { recursive: true, force: true });
        if (service) {
            service.kill('SIGTERM');
            let timer;
            const timeout = new Promise(resolve => { timer = setTimeout(resolve, 5000, 'timeout'); });
            const code = await Promise.race([exited, timeout]);
            clearTimeout(timer);
            if (code === 'timeout') { service.kill('SIGKILL'); await exited; }
            assert.equal(code, 0, stderr || 'service shutdown failed');
        }
        if (base) await fs.rm(base, { recursive: true });
    }
}
run().catch(error => { console.error(error); process.exitCode = 1; });

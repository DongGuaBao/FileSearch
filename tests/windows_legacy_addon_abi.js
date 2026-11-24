'use strict';
// Run an unchanged prebuilt addon with the new DLL in an isolated staging dir.
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const addonPath = path.resolve(process.argv[2]);
const resultPath = path.resolve(process.argv[3]);
const root = path.dirname(addonPath);
const addon = require(addonPath);
const fsSync = require('node:fs');
const checkpoint = step => fsSync.writeFileSync(resultPath, JSON.stringify({ ok: false, pending_step: step }, null, 2));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function run() {
    let original, renamed;
    try {
        assert.ok(addon.isElevated(), 'run this test elevated');
        checkpoint('configure');
        assert.equal(addon.directConfigure({ cache_directory: path.join(root, 'cache'),
            drives: [root[0]], monitor_enabled: true, mode: 'ngram' }), true);
        checkpoint('start_mft'); assert.equal(addon.directStartMft(), true);
        checkpoint('start_search');
        assert.equal(addon.directStartSearch(), true);
        checkpoint('status');
        assert.equal(addon.directStatus().ready, true);
        checkpoint('release_facts');
        addon.directReleaseFacts();
        const token = 'legacy_abi_' + crypto.randomUUID().replaceAll('-', '');
        original = path.join(root, token + '_original.txt');
        renamed = path.join(root, token + '_renamed.txt');
        const has = (query, target) => addon.directSearch(query, { maxResults: 20 }).results
            .some(entry => entry.path.toLowerCase() === target.toLowerCase());
        async function until(predicate, message) {
            const end = Date.now() + 15000;
            while (!predicate()) { assert.ok(Date.now() < end, message); await sleep(50); }
        }
        await fs.writeFile(original, 'ABI fixture');
        checkpoint('search_create');
        await until(() => has(token + '_original', original), 'old binary could not search new file');
        await fs.rename(original, renamed);
        await until(() => has(token + '_renamed', renamed), 'old binary could not search renamed file');
        assert.equal(has(token + '_original', original), false);
        await fs.unlink(renamed);
        await until(() => !has(token + '_renamed', renamed), 'deleted fixture remains searchable');
        await fs.writeFile(resultPath, JSON.stringify({ ok: true, unchanged_addon: addonPath,
            verified: ['load', 'configure', 'mft', 'search', 'status', 'released_facts', 'create', 'rename', 'delete', 'stop'] }, null, 2));
    } catch (error) {
        await fs.writeFile(resultPath, JSON.stringify({ ok: false, error: error.stack }, null, 2));
        process.exitCode = 1;
    } finally {
        addon.stopDirect();
        for (const target of [original, renamed]) if (target) await fs.rm(target, { force: true });
    }
}
run().catch(error => { console.error(error); process.exitCode = 1; });

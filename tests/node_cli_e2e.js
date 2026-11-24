'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const { spawn, execFile } = require('node:child_process');
const { promisify } = require('node:util');
const execute = promisify(execFile);
const cli = path.resolve(__dirname, '../clients/node/cli.js');

async function main() {
    assert.equal(process.platform, 'linux', 'Run this isolated service test on Linux');
    assert.equal(process.getuid(), 0, 'root required for fanotify');
    const base = await fs.mkdtemp('/home/donggua/filesearch-cli-e2e-');
    const root = path.join(base, 'files'), ipc = path.join(base, 'ipc');
    let service, finished, log = '';
    try {
        await fs.mkdir(root); await fs.mkdir(ipc);
        const fixture = path.join(root, 'cli fixture 中文.txt');
        await fs.writeFile(fixture, 'CLI test');
        service = spawn(process.argv[2], [root, ipc], { stdio: ['ignore', 'ignore', 'pipe'] });
        service.stderr.on('data', chunk => { log += chunk; });
        finished = new Promise(resolve => service.once('exit', resolve));
        service.on('error', error => { log += error.message; });
        const call = (...args) => execute(process.execPath, [cli, '--ipc-dir', ipc, ...args], { timeout: 10000 });
        const deadline = Date.now() + 10000;
        while (true) {
            try { if (JSON.parse((await call('--status')).stdout).ready) break; } catch {}
            assert.equal(service.exitCode, null, log);
            assert.ok(Date.now() < deadline, 'Service readiness timeout: ' + log);
            await new Promise(resolve => setTimeout(resolve, 50));
        }
        assert.equal((await call('--scope', root, 'cli fixture')).stdout.trim(), fixture);
        const response = JSON.parse((await call('--json', '--phase', 'both', 'zhongwen')).stdout);
        assert.equal(response.results.length, 1);
        assert.equal(typeof response.results[0].id, 'string');
        assert.equal((await call('no-such-cli-fixture-xyz')).stdout, '');
        console.log('CLI real Linux service test passed');
    } finally {
        if (service && service.exitCode === null) { service.kill('SIGTERM'); await finished; }
        await fs.rm(base, { recursive: true, force: true });
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });

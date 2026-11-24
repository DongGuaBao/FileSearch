'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { parseArgs, run } = require('../clients/node/cli');

test('CLI argument parsing and phase mapping', () => {
    const parsed = parseArgs(['--scope', '/home/with spaces', '--limit', '7', '--phase', 'both',
        '--ipc-dir', '/state', '--endpoint', '/pipe', '--timeout', '2000', 'hello', 'world']);
    assert.equal(parsed.query, 'hello world');
    assert.deepEqual(parsed.search, { scope_path: '/home/with spaces', max_results: 7, phase: 'exact_then_fuzzy' });
    assert.deepEqual(parsed.connection, { ipcDirectory: '/state', searchEndpoint: '/pipe', timeoutMs: 2000 });
    assert.equal(parseArgs(['--', '-filename']).query, '-filename');
    assert.equal(parseArgs(['--help']).help, true);
    assert.equal(parseArgs(['--status']).status, true);
    for (const args of [[], ['--bad'], ['--scope'], ['--limit', '0', 'x'], ['--limit', '-1', 'x'],
        ['--limit', '1.2', 'x'], ['--limit', '10001', 'x'], ['--timeout', 'Infinity', 'x'],
        ['--phase', 'bad', 'x'], ['--status', 'x']]) assert.throws(() => parseArgs(args));
});

function output() {
    let stdout = '', stderr = '';
    return { stdout: { write: text => { stdout += text; } }, stderr: { write: text => { stderr += text; } },
        text: () => ({ stdout, stderr }) };
}
test('CLI search, JSON precision, empty result and terminal escaping', async () => {
    const io = output();
    const clientFactory = () => ({ search: async (query, options) => {
        assert.equal(query, '中文'); assert.equal(options.phase, 'fuzzy_only');
        return { ok: true, results: [{ path: '/home/a\n\x1b[31m.txt', id: 18446744073709551615n }] };
    } });
    assert.equal(await run(['--phase', 'fuzzy', '中文'], { ...io, clientFactory }), 0);
    assert.equal(io.text().stdout, '/home/a\\u000a\\u001b[31m.txt\n');
    const json = output();
    assert.equal(await run(['--json', '--phase', 'fuzzy', '中文'], { ...json, clientFactory }), 0);
    assert.equal(JSON.parse(json.text().stdout).results[0].id, '18446744073709551615');
    const empty = output();
    assert.equal(await run(['x'], { ...empty, clientFactory: () => ({ search: async () => ({ results: [] }) }) }), 0);
    assert.equal(empty.text().stdout, ''); assert.match(empty.text().stderr, /0 条结果/);
    const stale = output();
    assert.equal(await run(['x'], { ...stale, clientFactory: () => ({ search: async () => ({ stale: true, results: [] }) }) }), 0);
    assert.match(stale.text().stderr, /缓存结果/);
});
test('CLI status, help and actionable errors', async () => {
    const io = output();
    assert.equal(await run(['--status'], { ...io, clientFactory: () => ({ status: async () => ({ ready: true }) }) }), 0);
    assert.equal(JSON.parse(io.text().stdout).ready, true);
    const help = output();
    assert.equal(await run(['--help'], { ...help, clientFactory: () => { throw new Error('should not connect'); } }), 0);
    assert.match(help.text().stdout, /Windows \/ Linux/);
    for (const code of ['ENOENT', 'EACCES']) {
        const error = output();
        assert.equal(await run(['x'], { ...error, clientFactory: () => ({ search: async () => {
            throw Object.assign(new Error('test error'), { code });
        } }) }), 1);
        assert.match(error.text().stderr, code === 'EACCES' ? /sudo/ : /服务已启动/);
    }
    assert.equal(await run([], output()), 2);
});

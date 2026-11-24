'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const { FileSearchClient } = require('../clients/node');
const json = require('../clients/node/lossless-json');

test('uint64 integers round-trip exactly without rewriting string content', () => {
    const text = '{"position":{"epoch":18446744073709551615,"sequence":9007199254740993},"id":42,"path":"id 18446744073709551615 \\"quoted\\""}';
    // Build escaped path independently so the fixture is valid JSON.
    const valid = text.slice(0, text.indexOf(',"path"')) + ',"path":' + JSON.stringify('id 18446744073709551615 "quoted"') + '}';
    const parsed = json.parse(valid);
    assert.equal(parsed.position.epoch, 18446744073709551615n);
    assert.equal(parsed.position.sequence, 9007199254740993n);
    assert.equal(parsed.id, 42n);
    assert.equal(parsed.path, 'id 18446744073709551615 "quoted"');
    assert.deepEqual(json.parse(json.stringify(parsed)), parsed);
    assert.throws(() => json.parse('{"id":18446744073709551616}'));
    assert.throws(() => json.parse('{"epoch":-1}'));
    assert.throws(() => json.parse('{"sequence":1e20}'));
    assert.equal(json.stringify([, undefined, 1n]), '[null,null,1]');
    const circular = {}; circular.self = circular;
    assert.throws(() => json.stringify(circular));
});

async function fixture(t, handler) {
    const root = await fs.mkdtemp(path.join(process.platform === 'linux' ? '/home/donggua' : os.tmpdir(), 'filesearch-node-'));
    const endpoint = process.platform === 'win32' ? '\\\\.\\pipe\\filesearch-test-' + crypto.randomUUID() : path.join(root, 'test.sock');
    const peers = new Set();
    const server = net.createServer(socket => {
        peers.add(socket); socket.on('error', () => {}); socket.on('close', () => peers.delete(socket));
        let buffer = Buffer.alloc(0);
        socket.on('data', chunk => {
            buffer = Buffer.concat([buffer, chunk]);
            while (buffer.length >= 4 && buffer.length >= 4 + buffer.readUInt32LE()) {
                const size = buffer.readUInt32LE();
                const request = json.parse(buffer.subarray(4, size + 4).toString());
                buffer = buffer.subarray(size + 4);
                handler(socket, request);
            }
        });
    });
    await new Promise((resolve, reject) => { server.once('error', reject); server.listen(endpoint, resolve); });
    t.after(async () => {
        for (const socket of peers) socket.destroy();
        await new Promise(resolve => server.close(resolve));
        await fs.rm(root, { recursive: true });
    });
    return new FileSearchClient({ platform: 'linux', searchEndpoint: endpoint, eventsEndpoint: endpoint, timeoutMs: 500 });
}

function frame(value) {
    const body = Buffer.from(json.stringify(value));
    const header = Buffer.alloc(4); header.writeUInt32LE(body.length);
    return Buffer.concat([header, body]);
}

test('fragmented frames and coalesced events preserve precision', async t => {
    const client = await fixture(t, (socket, request) => {
        if (request.command === 'status') {
            const bytes = frame({ ok: true, position: { epoch: 18446744073709551615n, sequence: 0n } });
            for (const byte of bytes) socket.write(Buffer.from([byte]));
        } else {
            assert.equal(request.since.epoch, 18446744073709551615n);
            socket.write(Buffer.concat([
                frame({ type: 'subscribed', position: request.since }),
                frame({ type: 'change', action: 'create', id: 9007199254740993n }),
                frame({ type: 'resync_required', position: request.since }),
            ]));
        }
    });
    const status = await client.status();
    const events = [];
    for await (const event of client.subscribeChanges({ since: status.position })) events.push(event);
    assert.deepEqual(events.map(event => event.type), ['subscribed', 'change', 'resync_required']);
    assert.equal(events[1].id, 9007199254740993n);
});

test('snapshot is demand-paged and retains its original cursor', async t => {
    let next = 0;
    const position = { epoch: 9007199254740993n, sequence: 12n };
    const client = await fixture(t, (socket, request) => {
        const index = request.command === 'snapshot' ? 0 : ++next;
        socket.write(frame({ ok: true, type: 'snapshot_page', page_index: index, position,
            entries: [{ id: BigInt(index + 1) }], done: index === 2 }));
    });
    const iterator = client.snapshot();
    assert.equal((await iterator.next()).value.page_index, 0);
    assert.equal(next, 0, 'client prefetched pages while consumer was paused');
    assert.equal((await iterator.next()).value.page_index, 1);
    assert.equal((await iterator.next()).value.page_index, 2);
    assert.equal((await iterator.next()).done, true);
});

test('rejects oversized, truncated and invalid UTF-8 responses', async t => {
    for (const bytes of [Buffer.from([1, 0, 16, 0]), Buffer.from([2, 0, 0, 0, 123]), Buffer.from([1, 0, 0, 0, 255])]) {
        const client = await fixture(t, socket => socket.end(bytes));
        await assert.rejects(client.status());
    }
});

test('abort cancels idle subscription and request timeout is bounded', async t => {
    const client = await fixture(t, (socket, request) => {
        if (request.command === 'subscribe_changes') socket.write(frame({ type: 'subscribed' }));
    });
    await assert.rejects(client.status(), error => error.code === 'timeout');
    const abort = new AbortController();
    const iterator = client.subscribeChanges({}, { signal: abort.signal });
    assert.equal((await iterator.next()).value.type, 'subscribed');
    const pending = iterator.next(); abort.abort();
    await assert.rejects(pending, error => error.name === 'AbortError');
});

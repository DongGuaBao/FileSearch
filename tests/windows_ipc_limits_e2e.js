'use strict';
// Uses only the additive events endpoint; production journals/cache are untouched.
const assert = require('node:assert/strict');
const net = require('node:net');
const { FileSearchClient } = require('../clients/node');
const endpoint = '\\\\.\\pipe\\FileSearch.Events';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const sockets = new Set();
async function connect(allowRejected = false) {
    const socket = net.createConnection(endpoint);
    sockets.add(socket);
    socket.on('error', () => {});
    const frames = [];
    let pending = Buffer.alloc(0);
    socket.on('data', bytes => {
        pending = Buffer.concat([pending, bytes]);
        while (pending.length >= 4 && pending.length >= 4 + pending.readUInt32LE(0)) {
            const length = pending.readUInt32LE(0);
            frames.push(JSON.parse(pending.subarray(4, 4 + length).toString('utf8')));
            pending = pending.subarray(4 + length);
        }
    });
    await new Promise((resolve, reject) => { socket.once('connect', resolve); socket.once('error', error => allowRejected ? resolve() : reject(error)); });
    return { socket, frames };
}
function send(socket, value) {
    const body = Buffer.from(JSON.stringify(value));
    const header = Buffer.alloc(4); header.writeUInt32LE(body.length);
    socket.write(header); socket.write(body);
}
async function until(predicate, message, timeout = 5000) {
    const deadline = Date.now() + timeout;
    while (!predicate()) { assert.ok(Date.now() < deadline, message); await sleep(20); }
}
async function subscribe() {
    const client = await connect();
    send(client.socket, { command: 'subscribe_changes', include_files: false, include_directories: false });
    await until(() => client.frames.length || client.socket.destroyed, 'subscription stalled');
    assert.equal(client.frames[0]?.type, 'subscribed');
    return client;
}
async function run() {
    const client = new FileSearchClient();
    const deadline = Date.now() + 120000;
    while (!(await client.status()).ready) { assert.ok(Date.now() < deadline); await sleep(100); }
    try {
        // Invalid lengths and incomplete requests must release their workers.
        for (const length of [0, 1024 * 1024 + 1, 512]) {
            const { socket } = await connect();
            const header = Buffer.alloc(4); header.writeUInt32LE(length); socket.write(header);
            await until(() => socket.destroyed, 'malformed/incomplete request retained a worker');
        }
        for (const request of [{ command: 'bad' }, { command: 'subscribe_changes', actions: ['bad'] },
            { command: 'subscribe_changes', version: 99 }]) {
            const { socket, frames } = await connect(); send(socket, request);
            await until(() => frames.length, 'error response lost');
            assert.equal(frames[0].type, 'error'); socket.destroy();
        }
        const idle = await connect();
        await until(() => idle.socket.destroyed, 'idle request did not time out');
        // Hold all 32 workers with acknowledged, idle subscriptions.
        const held = [];
        for (let i = 0; i < 32; ++i) held.push(await subscribe());
        const excess = await connect(true);
        await until(() => excess.socket.destroyed, '33rd events connection was not rejected');
        assert.equal((await client.status()).ready, true, 'events saturation blocked legacy search endpoint');
        held[0].socket.destroy();
        await sleep(750);
        const replacement = await subscribe(); replacement.socket.destroy();
        for (const entry of held) entry.socket.destroy();
        await sleep(750);
        // Repeated idle disconnects must not leak subscriber slots.
        for (let i = 0; i < 64; ++i) { const entry = await subscribe(); entry.socket.destroy(); await sleep(25); }
        await sleep(750);
        if (process.argv.includes('--shutdown')) {
            for (let i = 0; i < 8; ++i) await subscribe();
            // Include a partially framed request during stop.
            const pending = await connect(); const header = Buffer.alloc(4); header.writeUInt32LE(100); pending.socket.write(header);
            await client.request({ command: 'shutdown' });
            await until(() => [...sockets].every(socket => socket.destroyed), 'shutdown retained events connections');
        }
        console.log('windows_ipc_limits_e2e passed');
    } finally { for (const socket of sockets) socket.destroy(); }
}
run().catch(error => { console.error(error); process.exitCode = 1; });

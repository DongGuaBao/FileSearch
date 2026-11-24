'use strict';
const net = require('node:net');
const path = require('node:path');
const { TextDecoder } = require('node:util');
const json = require('./lossless-json');
const MAX_FRAME = 1024 * 1024;

class FileSearchError extends Error {
    constructor(message, code, response) {
        super(message);
        this.name = 'FileSearchError'; this.code = code; this.response = response;
    }
}

function writeFrame(socket, request) {
    const encoded = json.stringify(request);
    if (encoded === undefined) throw new TypeError('Request must be JSON');
    const body = Buffer.from(encoded, 'utf8');
    if (!body.length || body.length > MAX_FRAME) throw new FileSearchError('Invalid request frame size', 'invalid_frame');
    const header = Buffer.alloc(4); header.writeUInt32LE(body.length);
    // The legacy Windows search pipe uses message mode and reads its header
    // with one ReadFile(4). Preserve that message boundary; combining header
    // and body would produce ERROR_MORE_DATA and disconnect without a reply.
    socket.write(header);
    socket.write(body);
}

async function* readFrames(socket) {
    const header = Buffer.alloc(4);
    let headerBytes = 0, body, bodyBytes = 0;
    const decoder = new TextDecoder('utf-8', { fatal: true });
    // Pause synchronously in the data callback. Windows DisconnectNamedPipe
    // can report read EPIPE immediately after a complete response; a generic
    // Readable iterator may otherwise discard buffered response bytes.
    let queued, ended = false, failure, wake;
    const notify = () => { if (wake) { const resolve = wake; wake = undefined; resolve(); } };
    const onData = chunk => { socket.pause(); queued = chunk; notify(); };
    const onEnd = () => { ended = true; notify(); };
    const onError = error => {
        if (process.platform === 'win32' && error.code === 'EPIPE') ended = true;
        else failure = error;
        notify();
    };
    socket.on('data', onData); socket.on('end', onEnd); socket.on('close', onEnd); socket.on('error', onError);
    socket.pause();
    try { for (;;) {
        while (!queued && !failure && !ended) {
            const available = new Promise(resolve => { wake = resolve; });
            socket.resume();
            await available;
        }
        if (!queued) {
            if (failure) throw failure;
            break;
        }
        const chunk = queued; queued = undefined;
        let offset = 0;
        while (offset < chunk.length) {
            if (!body) {
                const count = Math.min(4 - headerBytes, chunk.length - offset);
                chunk.copy(header, headerBytes, offset, offset + count);
                headerBytes += count; offset += count;
                if (headerBytes < 4) continue;
                const length = header.readUInt32LE();
                if (!length || length > MAX_FRAME) throw new FileSearchError('Invalid response frame size', 'invalid_frame');
                body = Buffer.alloc(length); bodyBytes = 0;
            }
            const count = Math.min(body.length - bodyBytes, chunk.length - offset);
            chunk.copy(body, bodyBytes, offset, offset + count);
            bodyBytes += count; offset += count;
            if (bodyBytes === body.length) {
                const response = json.parse(decoder.decode(body));
                body = undefined; headerBytes = 0;
                yield response;
            }
        }
    }
    if (body || headerBytes) throw new FileSearchError('Truncated response frame', 'invalid_frame');
    } finally {
        socket.off('data', onData); socket.off('end', onEnd); socket.off('close', onEnd); socket.off('error', onError);
        socket.pause();
    }
}

function check(response) {
    if (response.ok === false || response.type === 'error' || response.type === 'resync_required')
        throw new FileSearchError(response.error || response.type || 'Request failed', response.code || response.type || 'request_failed', response);
    return response;
}

class FileSearchClient {
    constructor({ platform = process.platform, ipcDirectory = '/var/lib/filesearch', searchEndpoint,
        eventsEndpoint, timeoutMs = 5000 } = {}) {
        if (!['win32', 'linux'].includes(platform)) throw new FileSearchError('Unsupported platform', 'unsupported');
        if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) throw new TypeError('timeoutMs must be positive');
        this.platform = platform; this.timeoutMs = timeoutMs;
        this.searchEndpoint = searchEndpoint || (platform === 'win32' ? '\\\\.\\pipe\\FileSearch' : path.join(ipcDirectory, 'search.sock'));
        this.eventsEndpoint = eventsEndpoint || (platform === 'win32' ? '\\\\.\\pipe\\FileSearch.Events' : path.join(ipcDirectory, 'events.sock'));
    }
    async _connect(endpoint, signal) {
        const socket = net.createConnection({ path: endpoint, signal });
        // Do not begin OS reads until the frame reader is attached. A native
        // Windows server may flush/disconnect before buffered Readable data is
        // handed to a listener, clearing it when EPIPE destroys the stream.
        socket.pause();
        // Preserve errors arriving between connect resolution and attaching the
        // async iterator; Readable retains the error for that iterator, too.
        socket.on('error', () => {});
        socket.setTimeout(this.timeoutMs, () => socket.destroy(new FileSearchError('Connection timed out', 'timeout')));
        try {
            await new Promise((resolve, reject) => {
                const failed = error => { socket.off('connect', connected); reject(error); };
                const connected = () => { socket.off('error', failed); resolve(); };
                socket.once('connect', connected); socket.once('error', failed);
            });
            return socket;
        } catch (error) { socket.destroy(); throw error; }
    }
    async request(request, { signal } = {}) {
        const socket = await this._connect(this.searchEndpoint, signal);
        try {
            writeFrame(socket, request);
            for await (const response of readFrames(socket)) return check(response);
            throw new FileSearchError('Connection closed without response', 'connection_closed');
        } finally { socket.destroy(); }
    }
    status(options) { return this.request({ command: 'status' }, options); }
    search(query, options = {}, transport = {}) {
        const request = { phase: 'exact_only', ...options, query };
        delete request.command; // Preserve Windows' non-exclusive search path.
        return this.request(request, transport).then(response => {
            if (this.platform === 'win32' && Array.isArray(response.results))
                response.results = response.results.map(entry => ({ ...entry, id: entry.id ?? entry.frn }));
            return response;
        });
    }
    async *subscribeChanges(options = {}, { signal } = {}) {
        const socket = await this._connect(this.eventsEndpoint, signal);
        try {
            writeFrame(socket, { ...options, command: 'subscribe_changes', version: 1 });
            let subscribed = false;
            for await (const response of readFrames(socket)) {
                if (!subscribed) {
                    check(response);
                    if (response.type !== 'subscribed') throw new FileSearchError('Missing subscription acknowledgement', 'invalid_response');
                    subscribed = true; socket.setTimeout(0);
                }
                yield response;
                if (response.type === 'resync_required' || response.type === 'error') return;
            }
            throw new FileSearchError('Subscription connection closed', 'connection_closed');
        } finally { socket.destroy(); }
    }
    async *snapshot(options = {}, { signal } = {}) {
        const socket = await this._connect(this.platform === 'win32' ? this.eventsEndpoint : this.searchEndpoint, signal);
        try {
            writeFrame(socket, { ...options, command: 'snapshot', version: 1 });
            let index = 0, position;
            for await (const response of readFrames(socket)) {
                check(response);
                if (response.type !== 'snapshot_page' || response.page_index !== index++ ||
                    !Array.isArray(response.entries) || typeof response.done !== 'boolean')
                    throw new FileSearchError('Invalid snapshot page', 'invalid_response');
                if (!position) position = response.position;
                if (typeof position?.epoch !== 'bigint' || position.epoch === 0n || typeof position.sequence !== 'bigint')
                    throw new FileSearchError('Missing snapshot position', 'invalid_response');
                if (response.position?.epoch !== position?.epoch || response.position?.sequence !== position?.sequence)
                    throw new FileSearchError('Snapshot position changed', 'invalid_response');
                yield response;
                if (response.done) return;
                writeFrame(socket, { command: 'snapshot_next' });
            }
            throw new FileSearchError('Incomplete snapshot', 'connection_closed');
        } finally { socket.destroy(); }
    }
}

module.exports = { FileSearchClient, FileSearchError };

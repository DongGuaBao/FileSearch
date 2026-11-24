"""Actual IPC framing, slow input, connection cap and bounded shutdown."""
import json
import os
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def connect(endpoint):
    peer = socket.socket(socket.AF_UNIX)
    peer.settimeout(4)
    peer.connect(str(endpoint))
    return peer


def receive(peer):
    def exact(count):
        data = b''
        while len(data) < count:
            chunk = peer.recv(count - len(data))
            if not chunk:
                raise EOFError('closed')
            data += chunk
        return data
    size, = struct.unpack('<I', exact(4))
    assert 0 < size <= 1024 * 1024
    return json.loads(exact(size))


def frame(request):
    body = json.dumps(request).encode()
    return struct.pack('<I', len(body)) + body


if os.geteuid() != 0:
    sys.exit(77)
with tempfile.TemporaryDirectory(prefix='filesearch-ipc-', dir='/home/donggua') as temporary:
    base = pathlib.Path(temporary)
    root, ipc = base / 'files', base / 'ipc'
    root.mkdir(); ipc.mkdir()
    (root / 'retained.txt').write_text('must remain searchable')
    process = subprocess.Popen([sys.argv[1], str(root), str(ipc)], stderr=subprocess.PIPE)
    endpoint = ipc / 'search.sock'
    held = []
    try:
        deadline = time.monotonic() + 8
        while not endpoint.exists():
            assert process.poll() is None, process.stderr.read().decode()
            assert time.monotonic() < deadline
            time.sleep(.02)
        # Split at every byte boundary of the header and body.
        while True:
            with connect(endpoint) as peer:
                peer.sendall(frame({'command': 'status'}))
                if receive(peer)['ready']:
                    break
            assert process.poll() is None and time.monotonic() < deadline
            time.sleep(.01)
        with connect(endpoint) as peer:
            for byte in frame({'query': 'retained'}):
                peer.sendall(bytes([byte]))
            assert len(receive(peer)['results']) == 1
        for invalid in [struct.pack('<I', 0), struct.pack('<I', 1024 * 1024 + 1),
                        struct.pack('<I', 1) + b'{', frame({'query': 'x', 'phase': 'unknown'})]:
            with connect(endpoint) as peer:
                peer.sendall(invalid)
                assert not receive(peer)['ok'], 'malformed request accepted'
        with connect(endpoint) as peer:
            peer.sendall(struct.pack('<I', 10) + b'{}')
            peer.shutdown(socket.SHUT_WR)
            assert not receive(peer)['ok'], 'truncated body accepted'
        # Sending occasional body bytes must not reset the whole-transfer
        # deadline and retain a worker forever (SO_RCVTIMEO alone would).
        with connect(endpoint) as peer:
            peer.sendall(struct.pack('<I', 10000))
            started = time.monotonic()
            for unused in range(8):
                try:
                    peer.sendall(b' ')
                except (BrokenPipeError, ConnectionResetError):
                    break
                time.sleep(.35)
            try:
                response = receive(peer)
                assert not response['ok']
            except (EOFError, ConnectionResetError):
                pass
            assert time.monotonic() - started < 3.5, 'slow body retained worker past transfer bound'
        # Hold all workers with incomplete headers; the 33rd must not create
        # an unbounded thread. Measure the actual service's thread count.
        for unused in range(32):
            peer = connect(endpoint)
            peer.sendall(b'\x10')
            held.append(peer)
        end = time.monotonic() + 1.5
        while True:
            status = pathlib.Path(f'/proc/{process.pid}/status').read_text()
            threads = next(int(line.split()[1]) for line in status.splitlines() if line.startswith('Threads:'))
            # Main acceptor + collector + at most 32 client workers.
            assert threads <= 34, f'connection cap bypass: threads={threads}'
            if threads == 34:
                break
            assert time.monotonic() < end, f'fixture did not saturate: threads={threads}'
            time.sleep(.01)
        with connect(endpoint) as extra:
            try:
                extra.sendall(frame({'command': 'status'}))
                extra.recv(1)
            except (ConnectionResetError, BrokenPipeError):
                pass
        for peer in held:
            peer.close()
        held.clear()
        deadline = time.monotonic() + 4
        while True:
            try:
                with connect(endpoint) as peer:
                    peer.sendall(frame({'query': 'retained'}))
                    assert len(receive(peer)['results']) == 1
                break
            except (EOFError, ConnectionResetError, BrokenPipeError):
                assert time.monotonic() < deadline, 'connection slots never recovered'
                time.sleep(.03)
        for unused in range(12):
            peer = connect(endpoint)
            peer.sendall(struct.pack('<I', 10000) + b' ')
            held.append(peer)
        started = time.monotonic()
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=4)
        assert process.returncode == 0, process.stderr.read().decode()
        assert time.monotonic() - started < 3, 'shutdown blocked behind incomplete clients'
        assert not endpoint.exists() and not (ipc / 'events.sock').exists()
        print('linux_ipc_limits_e2e passed')
    finally:
        for peer in held:
            peer.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()

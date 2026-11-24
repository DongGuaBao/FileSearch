"""Real startup/live directory replacement races; no mocked notifications."""
import json
import os
import pathlib
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time


def send(peer, request):
    body = json.dumps(request).encode()
    peer.sendall(struct.pack('<I', len(body)) + body)


def receive(peer):
    def exact(count):
        result = b''
        while len(result) < count:
            chunk = peer.recv(count - len(result))
            assert chunk, 'service closed connection'
            result += chunk
        return result
    size, = struct.unpack('<I', exact(4))
    assert 0 < size <= 1024 * 1024
    return json.loads(exact(size))


def exchange(path, request):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(5)
        peer.connect(str(path))
        send(peer, request)
        return receive(peer)


if os.geteuid() != 0:
    sys.exit(77)
with tempfile.TemporaryDirectory(prefix='filesearch-scan-race-', dir='/home/donggua') as temporary:
    base = pathlib.Path(temporary)
    root, ipc = base / 'files', base / 'ipc'
    root.mkdir()
    ipc.mkdir()
    for i in range(100):
        (root / f'stable_{i}.txt').write_text('must survive every reconciliation')
    stop = threading.Event()
    failures = []
    cycles = [0]

    def churn():
        try:
            changing, outside = root / 'changing', base / 'outside'
            while not stop.is_set():
                changing.mkdir()
                # Enough entries to overlap a subtree scan, without a large
                # rename operation dominating the service's mutation work.
                for i in range(40):
                    (changing / f'racing_{i}.txt').write_text('racing')
                changing.rename(outside)
                for entry in outside.iterdir():
                    entry.unlink()
                outside.rmdir()
                # Alternate directory and symlink generations at one path.
                changing.symlink_to(base / 'not-indexed', target_is_directory=True)
                changing.unlink()
                cycles[0] += 1
        except BaseException as failure:
            failures.append(failure)

    writer = threading.Thread(target=churn)
    writer.start()
    process = subprocess.Popen([sys.argv[1], str(root), str(ipc)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 15
        while not (ipc / 'search.sock').exists():
            assert process.poll() is None, process.stderr.read().decode()
            assert time.monotonic() < deadline, 'startup scan never yielded to clients'
            time.sleep(.01)
        observed_not_ready = False
        old_position = None
        while True:
            assert process.poll() is None, process.stderr.read().decode()
            status = exchange(ipc / 'search.sock', {'command': 'status'})
            if old_position is None:
                old_position = status['position']
            if not status['ready']:
                response = exchange(ipc / 'search.sock', {'query': 'stable', 'max_results': 1000})
                if not response['ok']:
                    assert response['code'] == 'not_ready', response
                    observed_not_ready = True
            if status['scan_retries'] > 0 and observed_not_ready and cycles[0] >= 10:
                break
            assert time.monotonic() < deadline, f'fixture did not exercise deferred recovery: {status}, cycles={cycles[0]}'
            time.sleep(.01)
        stop.set()
        writer.join(timeout=5)
        assert not writer.is_alive() and not failures, failures
        deadline = time.monotonic() + 15
        expected = {os.fsencode(entry) for entry in root.rglob('*')}
        while True:
            assert process.poll() is None, process.stderr.read().decode()
            status = exchange(ipc / 'search.sock', {'command': 'status'})
            with sqlite3.connect(ipc / 'catalog.db') as database:
                actual = {bytes(row[0]) for row in database.execute('SELECT path FROM entries')}
            if status['ready'] and actual == expected:
                break
            assert time.monotonic() < deadline, f'no convergence: {status}, missing={expected-actual}, extra={actual-expected}'
            time.sleep(.02)
        results = exchange(ipc / 'search.sock', {'query': 'stable', 'max_results': 1000})
        assert results['ok'] and len(results['results']) == 100, results
        assert not exchange(ipc / 'search.sock', {'query': 'racing'})['results'], 'vanished entries remained searchable'
        with socket.socket(socket.AF_UNIX) as resumed:
            resumed.settimeout(5)
            resumed.connect(str(ipc / 'events.sock'))
            send(resumed, {'command': 'subscribe_changes', 'since': old_position})
            assert receive(resumed)['type'] == 'subscribed'
            assert receive(resumed)['type'] == 'resync_required', 'raced snapshot retained obsolete subscription epoch'
        # Recovery must leave live monitoring operational, not just a correct
        # one-off snapshot. Verify subsequent insert and delete durability.
        with socket.socket(socket.AF_UNIX) as events:
            events.settimeout(5)
            events.connect(str(ipc / 'events.sock'))
            send(events, {'command': 'subscribe_changes', 'actions': ['create', 'delete']})
            assert receive(events)['type'] == 'subscribed'
            live = root / 'post_recovery_live.txt'
            live.write_text('live')
            created = receive(events)
            assert created['action'] == 'create' and created['path'] == str(live), created
            assert exchange(ipc / 'search.sock', {'query': 'post_recovery'})['results'], 'live insert not searchable'
            live.unlink()
            deleted = receive(events)
            assert deleted['action'] == 'delete' and deleted['id'] == created['id'], deleted
            assert not exchange(ipc / 'search.sock', {'query': 'post_recovery'})['results'], 'live deletion not searchable'
        with sqlite3.connect(ipc / 'catalog.db') as database:
            actual = {bytes(row[0]) for row in database.execute('SELECT path FROM entries')}
        assert actual == expected, 'live monitoring did not maintain durable catalog after recovery'
        print(f'linux_scan_race_e2e passed: cycles={cycles[0]}, retries={status["scan_retries"]}')
    finally:
        stop.set()
        writer.join(timeout=5)
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError('service failed to shut down')
        assert process.returncode == 0, process.stderr.read().decode()

import json
import os
import pathlib
import socket
import sqlite3
import statistics
import struct
import subprocess
import sys
import tempfile
import time


def receive(sock):
    def exact(size):
        result = b''
        while len(result) < size:
            chunk = sock.recv(size - len(result))
            if not chunk:
                raise RuntimeError('incomplete frame')
            result += chunk
        return result
    size, = struct.unpack('<I', exact(4))
    assert 0 < size <= 1024 * 1024
    return json.loads(exact(size))


def connect(path, request):
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(10)
    sock.connect(str(path))
    body = json.dumps(request).encode()
    sock.sendall(struct.pack('<I', len(body)) + body)
    return sock


if os.geteuid() != 0:
    sys.exit(77)
count = int(sys.argv[2]) if len(sys.argv) > 2 else 100000
assert 100 <= count <= 1000000
with tempfile.TemporaryDirectory(prefix='filesearch-scale-', dir='/home/donggua') as temporary:
    base = pathlib.Path(temporary)
    root, ipc = base / 'files', base / 'ipc'
    root.mkdir()
    ipc.mkdir()
    for number in range(count):
        directory = root / f'group_{number // 1000}'
        if number % 1000 == 0:
            directory.mkdir()
        (directory / f'scale_report_{number}.txt').touch()
    started = time.monotonic()
    process = subprocess.Popen([sys.argv[1], str(root), str(ipc)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 120
        while not (ipc / 'search.sock').exists():
            if process.poll() is not None:
                raise RuntimeError(process.stderr.read().decode())
            assert time.monotonic() < deadline, 'service startup exceeded test bound'
            time.sleep(.02)
        while True:
            with connect(ipc / 'search.sock', {'command': 'status'}) as sock:
                if receive(sock)['ready']:
                    break
            assert process.poll() is None and time.monotonic() < deadline
            time.sleep(.02)
        startup_ms = (time.monotonic() - started) * 1000
        def search(query):
            with connect(ipc / 'search.sock', {'query': query, 'max_results': 50}) as sock:
                response = receive(sock)
                assert response['ok'], response
                return response['results']
        with sqlite3.connect(ipc / 'catalog.db') as database:
            assert database.execute('SELECT count(*) FROM entries').fetchone()[0] == count + (count + 999) // 1000
        durations = []
        for repetition in range(100):
            query_start = time.monotonic()
            result = search(f'scale_report_{count // 2 + repetition}.txt')
            assert len(result) == 1
            durations.append((time.monotonic() - query_start) * 1000)
        broad_start = time.monotonic()
        assert len(search('.txt')) == 50
        broad_ms = (time.monotonic() - broad_start) * 1000
        live_start = time.monotonic()
        with connect(ipc / 'events.sock', {'command': 'subscribe_changes', 'version': 1, 'actions': ['create', 'rename', 'delete']}) as events:
            assert receive(events)['type'] == 'subscribed'
            for repetition in range(50):
                live = root / f'live_scale_{repetition}.txt'
                live.write_text('live')
                event = receive(events)
                assert event['action'] == 'create' and event['path'] == str(live), event
                identifier = event['id']
                assert search(live.name)[0]['id'] == identifier
                moved = root / f'moved_scale_{repetition}.txt'
                live.rename(moved)
                event = receive(events)
                assert event['action'] == 'rename' and event['id'] == identifier and event['path'] == str(moved), event
                moved.unlink()
                event = receive(events)
                assert event['action'] == 'delete' and event['id'] == identifier, event
                assert not search(moved.name)
        live_ms = (time.monotonic() - live_start) * 1000
        with sqlite3.connect(ipc / 'catalog.db') as database:
            assert database.execute('PRAGMA integrity_check').fetchone() == ('ok',)
            assert database.execute('SELECT count(*) FROM entries').fetchone()[0] == count + (count + 999) // 1000
        status = pathlib.Path(f'/proc/{process.pid}/status').read_text()
        peak_kib = next(int(line.split()[1]) for line in status.splitlines() if line.startswith('VmHWM:'))
        print(json.dumps({'files': count, 'startup_ms': round(startup_ms, 2), 'peak_rss_kib': peak_kib,
            'search_median_ms': round(statistics.median(durations), 3), 'search_p95_ms': round(sorted(durations)[94], 3),
            'broad_search_ms': round(broad_ms, 2), 'live_50_create_rename_delete_ms': round(live_ms, 2)}))
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError('service shutdown timed out')
        assert process.returncode == 0, process.stderr.read().decode()

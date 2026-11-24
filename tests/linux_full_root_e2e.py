"""Opt-in root-scope WSL smoke test; creates only an owned ext4 fixture/state."""
import json
import os
import pathlib
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time


def send(peer, body):
    encoded = json.dumps(body).encode()
    peer.sendall(struct.pack('<I', len(encoded)) + encoded)


def receive(peer):
    def exact(count):
        result = b''
        while len(result) < count:
            part = peer.recv(count - len(result))
            assert part, 'incomplete frame'
            result += part
        return result
    length, = struct.unpack('<I', exact(4))
    assert 0 < length <= 1024 * 1024
    return json.loads(exact(length))


def connect(path):
    peer = socket.socket(socket.AF_UNIX)
    peer.settimeout(10)
    peer.connect(str(path))
    return peer


if os.geteuid() != 0:
    sys.exit(77)
with tempfile.TemporaryDirectory(prefix='filesearch-root-e2e-', dir='/home/donggua') as directory:
    base = pathlib.Path(directory)
    state = base / 'state'
    state.mkdir()
    fixture = base / 'files'
    fixture.mkdir()
    original = fixture / 'root_scope_original.txt'
    original.write_text('fixture')
    with tempfile.TemporaryFile() as log:
        service = subprocess.Popen([sys.argv[1], '/', str(state)], stdout=log, stderr=log)
        started = time.monotonic()
        try:
            def request(body):
                with connect(state / 'search.sock') as peer:
                    send(peer, body)
                    return receive(peer)
            deadline = started + 180
            while True:
                assert service.poll() is None, 'root service exited'
                try:
                    status = request({'command': 'status'})
                    if status['ready']:
                        break
                except (FileNotFoundError, ConnectionError):
                    pass
                assert time.monotonic() < deadline, 'root service did not become ready'
                time.sleep(.1)
            found = request({'query': 'root_scope_original', 'scope_path': str(fixture)})
            assert found['ok'] and len(found['results']) == 1, found
            identity = found['results'][0]['id']
            with connect(state / 'events.sock') as events:
                send(events, {'command': 'subscribe_changes', 'scope_path': str(fixture), 'actions': ['rename', 'delete']})
                assert receive(events)['type'] == 'subscribed'
                renamed = fixture / 'root_scope_renamed.txt'
                original.rename(renamed)
                change = receive(events)
                assert change['action'] == 'rename' and change['id'] == identity, change
                with connect(state / 'search.sock') as snapshot:
                    send(snapshot, {'command': 'snapshot', 'scope_path': str(fixture), 'include_directories': False})
                    page = receive(snapshot)
                    assert page.get('done') and len(page.get('entries', [])) == 1, page
                    assert page['entries'][0]['id'] == identity, page
                renamed.unlink()
                change = receive(events)
                assert change['action'] == 'delete' and change['id'] == identity, change
            # Inspect the complete durable catalog, not merely a limited query.
            with sqlite3.connect(state / 'catalog.db') as database:
                count = database.execute('SELECT count(*) FROM entries').fetchone()[0]
                for (raw,) in database.execute('SELECT path FROM entries'):
                    path = bytes(raw)
                    assert not (path.startswith(b'/mnt/') and len(path) >= 6 and
                                97 <= path[5] <= 122 and (len(path) == 6 or path[6:7] == b'/')), path
                    assert not path.startswith((b'/mnt/wslg/', b'/proc/', b'/sys/', b'/dev/')), path
                    assert not path.startswith(str(state).encode() + b'/'), path
                assert database.execute('PRAGMA integrity_check').fetchone() == ('ok',)
            print(json.dumps({'test': 'linux_full_root_e2e', 'entries': count,
                              'milliseconds': round((time.monotonic() - started) * 1000)}))
        finally:
            service.terminate()
            try:
                service.wait(timeout=10)
            except subprocess.TimeoutExpired:
                service.kill()
                service.wait()
                raise RuntimeError('root service shutdown timed out')
            if service.returncode != 0:
                log.seek(0)
                raise RuntimeError(log.read().decode(errors='replace'))

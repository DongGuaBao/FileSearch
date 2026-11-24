import contextlib
import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time


def send(peer, request):
    body = json.dumps(request).encode()
    peer.sendall(struct.pack('<I', len(body)) + body)


def receive(peer):
    def exact(size):
        result = b''
        while len(result) < size:
            chunk = peer.recv(size - len(result))
            assert chunk, 'connection closed before response'
            result += chunk
        return result
    size, = struct.unpack('<I', exact(4))
    assert 0 < size <= 1024 * 1024, size
    return json.loads(exact(size))


def connect(path):
    peer = socket.socket(socket.AF_UNIX)
    peer.settimeout(5)
    peer.connect(str(path))
    return peer


def request(path, body):
    with connect(path) as peer:
        send(peer, body)
        return receive(peer)


def pages(peer, first):
    result = []
    position = first['position']
    index = 0
    while True:
        assert first['ok'] and first['type'] == 'snapshot_page', first
        assert first['page_index'] == index and first['position'] == position, first
        assert first['count'] == len(first['entries'])
        result.extend(first['entries'])
        if first['done']:
            return result, position
        send(peer, {'command': 'snapshot_next'})
        first = receive(peer)
        index += 1


if os.geteuid() != 0:
    sys.exit(77)
with tempfile.TemporaryDirectory(prefix='filesearch-snapshot-', dir='/home/donggua') as temporary:
    base = pathlib.Path(temporary)
    root, ipc = base / 'files', base / 'ipc'
    root.mkdir()
    ipc.mkdir()
    for i in range(250):
        (root / f'baseline_{i:03}.txt').write_text('baseline')
    scope, sibling = root / 'scope', root / 'scope-sibling'
    scope.mkdir()
    sibling.mkdir()
    (scope / 'scoped.txt').write_text('inside')
    (sibling / 'scoped.txt').write_text('outside')
    raw_scope = os.fsencode(root) + b'/raw_scope_\xff'
    os.mkdir(raw_scope)
    raw_child = raw_scope + b'/raw_child_\xfe.txt'
    with open(raw_child, 'wb') as output:
        output.write(b'raw')
    deep = root
    for i in range(5):
        deep = deep / (str(i) + 'd' * 100)
        deep.mkdir()
    for i in range(200):
        (deep / (f'{i:03}_' + 'f' * 200)).write_text('long path')
    initial = {os.fsencode(entry) for entry in root.rglob('*')}
    process = subprocess.Popen([sys.argv[1], str(root), str(ipc)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    mounted = None
    try:
        deadline = time.monotonic() + 10
        while not (ipc / 'search.sock').exists():
            assert process.poll() is None, process.stderr.read().decode()
            assert time.monotonic() < deadline
            time.sleep(.01)
        search_socket = ipc / 'search.sock'
        while not request(search_socket, {'command': 'status'})['ready']:
            assert process.poll() is None and time.monotonic() < deadline
            time.sleep(.01)
        if '--invalidate' in sys.argv:
            target = root / 'alias'
            target.mkdir()
            # The CTest command runs this branch in a private mount namespace.
            with connect(search_socket) as snapshot:
                send(snapshot, {'command': 'snapshot', 'page_size': 1})
                first = receive(snapshot)
                assert first['ok'] and not first['done']
                subprocess.run(['mount', '--bind', str(scope), str(target)], check=True)
                mounted = target
                deadline = time.monotonic() + 1.8
                while request(search_socket, {'command': 'status'})['position']['epoch'] == first['position']['epoch']:
                    assert time.monotonic() < deadline, 'mount change did not invalidate snapshot epoch'
                    time.sleep(.01)
                send(snapshot, {'command': 'snapshot_next'})
                invalidated = receive(snapshot)
                assert not invalidated['ok'] and invalidated['type'] == 'resync_required', invalidated
            deadline = time.monotonic() + 5
            while not request(search_socket, {'command': 'status'})['ready']:
                assert time.monotonic() < deadline
                time.sleep(.01)
            response = request(search_socket, {'command': 'snapshot', 'scope_path': str(target)})
            assert response['ok'] and response['done'], response
            assert {entry['path'] for entry in response['entries']} == {str(target), str(target / 'scoped.txt')}, response
        elif '--limits' in sys.argv:
            with contextlib.ExitStack() as resources:
                for _ in range(4):
                    peer = resources.enter_context(connect(search_socket))
                    send(peer, {'command': 'snapshot', 'page_size': 1})
                    assert receive(peer)['ok']
                busy = request(search_socket, {'command': 'snapshot'})
                assert not busy['ok'] and busy['code'] == 'snapshot_busy', busy
                assert request(search_socket, {'command': 'status'})['ready'], 'snapshot quota blocked status'
            deadline = time.monotonic() + 3
            while True:
                response = request(search_socket, {'command': 'snapshot', 'page_size': 1024})
                if response['ok']:
                    break
                assert response.get('code') == 'snapshot_busy' and time.monotonic() < deadline, response
                time.sleep(.01)
            with connect(search_socket) as idle:
                send(idle, {'command': 'snapshot', 'page_size': 1})
                assert receive(idle)['ok']
                time.sleep(2.3)
                assert not receive(idle)['ok'], 'idle snapshot was not released'
                assert idle.recv(1) == b'', 'idle snapshot connection remained open'
            with connect(search_socket) as bounded:
                send(bounded, {'command': 'snapshot', 'page_size': 1})
                assert receive(bounded)['ok']
                started = time.monotonic()
                while True:
                    time.sleep(1)
                    send(bounded, {'command': 'snapshot_next', 'page_size': 1})
                    response = receive(bounded)
                    if not response['ok']:
                        assert response['code'] == 'snapshot_expired', response
                        break
                    assert not response['done'] and time.monotonic() - started < 33, 'snapshot exceeded its hard deadline'
                assert 29 <= time.monotonic() - started < 34, 'snapshot deadline not enforced at 30 seconds'
            assert request(search_socket, {'command': 'status'})['ready']
        else:
            with connect(search_socket) as snapshot:
                send(snapshot, {'command': 'snapshot', 'page_size': 7})
                first = receive(snapshot)
                assert first['ok'] and not first['done'] and first['count'] == 7, first
                # Commit changes after the first page. The remaining pages must
                # still include both old names and the subsequently deleted row.
                deleted = root / 'baseline_200.txt'
                renamed = root / 'baseline_201.txt'
                destination = root / 'renamed_after_snapshot.txt'
                created = root / 'created_after_snapshot.txt'
                deleted.unlink()
                renamed.rename(destination)
                created.write_text('after snapshot')
                deadline = time.monotonic() + 5
                while not request(search_socket, {'query': 'created_after_snapshot'})['results']:
                    assert time.monotonic() < deadline
                    time.sleep(.01)
                entries, position = pages(snapshot, first)
            view = {bytes.fromhex(entry['path_bytes_hex']): entry['id'] for entry in entries}
            assert len(view) == len(entries) and set(view) == initial, 'paged snapshot was not immutable or complete'
            assert raw_child in view and raw_scope in view, 'snapshot lost raw filename bytes'
            with connect(ipc / 'events.sock') as events:
                send(events, {'command': 'subscribe_changes', 'since': position, 'actions': ['create', 'rename', 'delete']})
                assert receive(events)['type'] == 'subscribed'
                for _ in range(3):
                    event = receive(events)
                    assert event['type'] == 'change', event
                    path = bytes.fromhex(event['path_bytes_hex'])
                    if event['action'] == 'delete':
                        assert view.pop(path) == event['id']
                    elif event['action'] == 'rename':
                        old = bytes.fromhex(event['old_path_bytes_hex'])
                        assert view.pop(old) == event['id']
                        view[path] = event['id']
                    else:
                        assert path not in view
                        view[path] = event['id']
            actual = {os.fsencode(entry) for entry in root.rglob('*')}
            assert set(view) == actual, 'snapshot plus replay did not reconstruct current view'
            # The byte budget must split an oversized requested page without
            # skipping its pending row, even though page_size allows 1024 rows.
            with connect(search_socket) as snapshot:
                send(snapshot, {'command': 'snapshot', 'page_size': 1024})
                first = receive(snapshot)
                assert first['ok'] and not first['done'] and first['count'] < 1024, first
                entries, _ = pages(snapshot, first)
                assert {bytes.fromhex(entry['path_bytes_hex']) for entry in entries} == actual
            for filters, expected in (
                ({'scope_path': str(scope) + '/', 'include_directories': False}, {os.fsencode(scope / 'scoped.txt')}),
                ({'scope_path': str(scope), 'include_files': False}, {os.fsencode(scope)}),
                ({'scope_path_bytes_hex': raw_scope.hex(), 'include_directories': False}, {raw_child}),
                ({'include_files': False, 'include_directories': False}, set()),
            ):
                response = request(search_socket, {'command': 'snapshot', **filters})
                assert response['ok'] and response['done'], response
                assert {bytes.fromhex(entry['path_bytes_hex']) for entry in response['entries']} == expected, response
            for size in (0, -1, 1025, 1.5, '3'):
                assert not request(search_socket, {'command': 'snapshot', 'page_size': size})['ok'], size
            with connect(search_socket) as invalid:
                send(invalid, {'command': 'snapshot', 'page_size': 1})
                assert receive(invalid)['ok']
                send(invalid, {'command': 'snapshot_next', 'scope_path': str(scope)})
                assert not receive(invalid)['ok'], 'snapshot allowed scope change'
            with connect(search_socket) as resized:
                send(resized, {'command': 'snapshot', 'page_size': 1, 'scope_path': str(scope)})
                assert receive(resized)['count'] == 1
                send(resized, {'command': 'snapshot_next', 'page_size': 10})
                response = receive(resized)
                assert response['ok'] and response['done'] and response['count'] == 1, response
        print('linux_snapshot_e2e passed')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError('snapshot service failed to stop')
        assert process.returncode == 0, process.stderr.read().decode()
        if mounted is not None:
            subprocess.run(['umount', str(mounted)], check=True)

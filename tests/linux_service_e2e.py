import json
import contextlib
import os
import pathlib
import socket
import signal
import struct
import sqlite3
import subprocess
import sys
import tempfile
import time
import threading


def send(sock, request):
    body = json.dumps(request).encode()
    sock.sendall(struct.pack('<I', len(body)) + body)


def exact(sock, count):
    body = b''
    while len(body) < count:
        chunk = sock.recv(count - len(body))
        if not chunk:
            raise RuntimeError('socket closed')
        body += chunk
    return body


def receive(sock):
    size, = struct.unpack('<I', exact(sock, 4))
    assert 0 < size <= 1024 * 1024
    return json.loads(exact(sock, size))


def connect(path):
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(5)
    sock.connect(str(path))
    return sock


@contextlib.contextmanager
def extra_filesystem(base, root):
    if not {'--multi-filesystem', '--dynamic-mount'} & set(sys.argv):
        yield root
        return
    image = base / 'test.ext4'
    with image.open('wb') as output:
        output.truncate(64 * 1024 * 1024)
    subprocess.run(['mkfs.ext4', '-q', '-F', str(image)], check=True)
    target = root / 'second-disk'
    target.mkdir()
    subprocess.run(['mount', '-o', 'loop', str(image), str(target)], check=True)
    try:
        yield target
    finally:
        subprocess.run(['umount', str(target)], check=True)


if os.geteuid() != 0:
    sys.exit(77)
with tempfile.TemporaryDirectory(prefix='filesearch-e2e-', dir='/home/donggua') as tmp:
    base = pathlib.Path(tmp)
    root = base / 'files'
    root.mkdir()
    sockets = (root if '--nested-ipc' in sys.argv else base) / 'ipc'
    sockets.mkdir()
    resources = contextlib.ExitStack()
    writer = None
    writer_errors = []
    try:
        files = root if '--dynamic-mount' in sys.argv else resources.enter_context(extra_filesystem(base, root))
        process = subprocess.Popen([sys.argv[1], str(root), str(sockets)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except BaseException:
        resources.close()
        raise
    try:
        deadline = time.monotonic() + 10
        while not (sockets / 'search.sock').exists():
            if process.poll() is not None:
                raise RuntimeError(process.stderr.read().decode())
            if time.monotonic() > deadline:
                raise TimeoutError('service readiness')
            time.sleep(.02)

        def search(query, phase='exact_only'):
            deadline = time.monotonic() + 10
            while True:
                with connect(sockets / 'search.sock') as sock:
                    send(sock, {'query': query, 'phase': phase})
                    response = receive(sock)
                if response.get('code') == 'not_ready':
                    assert time.monotonic() < deadline, response
                    time.sleep(.02)
                    continue
                assert response['ok'], response
                return response['results']
        assert not search('catalog.db'), 'service database was indexed'
        (root / 'fuzzyreport.txt').write_text('fuzzy fixture')
        deadline = time.monotonic() + 5
        while not search('fuzzyreport'):
            assert time.monotonic() < deadline
            time.sleep(.02)
        assert not search('fuzzyreprot'), 'exact phase accepted typo'
        assert any(item['name'] == 'fuzzyreport.txt' for item in search('fuzzyreprot', 'fuzzy_only')), 'fuzzy phase missed typo'
        assert any(item['name'] == 'fuzzyreport.txt' for item in search('fuzzyreprot', 'exact_then_fuzzy')), 'combined phase missed typo'
        if '--nested-ipc' in sys.argv:
            hidden = sockets / 'excluded_transfer.txt'
            visible = root / 'excluded_transfer.txt'
            hidden.write_text('excluded directory boundary')
            assert not search('excluded_transfer'), 'excluded file was indexed'
            hidden.rename(visible)
            deadline = time.monotonic() + 5
            while not search('excluded_transfer'):
                assert time.monotonic() < deadline, 'move out of excluded directory missed'
                time.sleep(.02)
            visible.rename(hidden)
            deadline = time.monotonic() + 5
            while search('excluded_transfer'):
                assert time.monotonic() < deadline, 'move into excluded directory retained'
                time.sleep(.02)
            sibling = root / 'ipc-sibling'
            sibling.mkdir()
            (sibling / 'boundary_visible.txt').write_text('not excluded')
            deadline = time.monotonic() + 5
            while not search('boundary_visible'):
                assert time.monotonic() < deadline, 'exclusion ignored path boundary'
                time.sleep(.02)

        scoped = files / 'scope'
        scoped.mkdir()
        sibling = files / 'scope-sibling'
        sibling.mkdir()
        inside = scoped / 'filtered.txt'
        outside = sibling / 'filtered.txt'
        inside.write_text('rename fixture')
        deadline = time.monotonic() + 5
        while not search('filtered'):
            assert time.monotonic() < deadline
            time.sleep(.02)
        with connect(sockets / 'events.sock') as filtered:
            send(filtered, {'command': 'subscribe_changes', 'version': 1,
                'scope_path': str(scoped) + '/', 'actions': ['rename'], 'include_directories': False})
            assert receive(filtered)['type'] == 'subscribed'
            with connect(sockets / 'events.sock') as observer:
                send(observer, {'command': 'subscribe_changes', 'version': 1,
                    'scope_path': str(scoped), 'actions': ['rename']})
                observer_start = receive(observer)['position']
                inside.rename(outside)
                first = receive(filtered)
                second = receive(observer)
                assert first['position'] == second['position'], 'subscribers drained each other'
                assert first['old_path'] == str(inside), first
                outside.rename(inside)
                receive(filtered)
                receive(observer)
            with connect(sockets / 'events.sock') as replay:
                send(replay, {'command': 'subscribe_changes', 'version': 1,
                    'since': observer_start, 'actions': ['rename'], 'scope_path': str(scoped)})
                assert receive(replay)['type'] == 'subscribed'
                assert receive(replay)['position'] == first['position'], 'resume lost retained event'
                assert receive(replay)['path'] == str(inside), 'resume ordering wrong'
            (sibling / 'ignored.txt').write_text('excluded')
            (scoped / 'ignored.txt').write_text('action excluded')
            inside.rename(outside)
            event = receive(filtered)
            assert event['action'] == 'rename' and event['old_path'] == str(inside) and event['path'] == str(outside), event
            with sqlite3.connect(sockets / 'catalog.db') as database:
                assert database.execute('SELECT path FROM entries WHERE path=?', (str(inside).encode(),)).fetchone() is None, 'rename source not removed durably'
                assert database.execute('SELECT path FROM entries WHERE path=?', (str(outside).encode(),)).fetchone(), 'rename destination not durable'
            outside.rename(inside)
            event = receive(filtered)
            assert event['action'] == 'rename' and event['path'] == str(inside), event

        if '--dynamic-mount' in sys.argv:
            with connect(sockets / 'events.sock') as events:
                send(events, {'command': 'subscribe_changes', 'version': 1})
                assert receive(events)['type'] == 'subscribed'
                files = resources.enter_context(extra_filesystem(base, root))
                (files / 'mounted_baseline.txt').write_text('mount reconciliation')
                # Creating the mountpoint itself is a valid pre-mount event.
                while receive(events)['type'] != 'resync_required':
                    pass
                assert search('mounted_baseline'), 'new mount baseline missing'

        with connect(sockets / 'events.sock') as events:
            send(events, {'command': 'subscribe_changes', 'version': 1})
            assert receive(events)['type'] == 'subscribed'
            path = files / 'socket_live.txt'
            path.write_text('test')
            deadline = time.monotonic() + 5
            while not search('socket_live'):
                assert time.monotonic() < deadline
                time.sleep(.02)
            with sqlite3.connect(sockets / 'catalog.db') as database:
                assert database.execute('SELECT name FROM entries WHERE path=?', (str(path).encode(),)).fetchone() == (path.name.encode(),), 'create not durable'
            path.unlink()
            actions = set()
            while not {'create', 'delete'} <= actions:
                event = receive(events)
                if event.get('name') == path.name:
                    actions.add(event['action'])
            deadline = time.monotonic() + 5
            while search('socket_live'):
                assert time.monotonic() < deadline
                time.sleep(.02)
            with sqlite3.connect(sockets / 'catalog.db') as database:
                assert database.execute('SELECT path FROM entries WHERE path=?', (str(path).encode(),)).fetchone() is None, 'delete not durable'
        if '--dynamic-mount' in sys.argv:
            with connect(sockets / 'events.sock') as events:
                send(events, {'command': 'subscribe_changes', 'version': 1})
                assert receive(events)['type'] == 'subscribed'
                resources.close()
                assert receive(events)['type'] == 'resync_required'
                assert not search('mounted_baseline'), 'unmounted filesystem remains indexed'
        if '--overflow' in sys.argv:
            stale = root / 'overflow_stale.txt'
            stale.write_text('must disappear')
            deadline = time.monotonic() + 5
            while not search('overflow_stale'):
                assert time.monotonic() < deadline
                time.sleep(.02)
            with connect(sockets / 'events.sock') as events:
                send(events, {'command': 'subscribe_changes', 'version': 1})
                assert receive(events)['type'] == 'subscribed'
                process.send_signal(signal.SIGSTOP)
                try:
                    capacity = int(pathlib.Path('/proc/sys/fs/fanotify/max_queued_events').read_text())
                    assert capacity <= 100000, 'unexpectedly large queue for bounded overflow fixture'
                    for i in range(capacity + 100):
                        churn = root / f'overflow_churn_{i}.txt'
                        churn.write_text('churn')
                        churn.unlink()
                    stale.unlink()
                    for i in range(25):
                        (root / f'overflow_survivor_{i}.txt').write_text('survivor')
                finally:
                    if '--concurrent-recovery' in sys.argv:
                        def mutate_during_recovery():
                            try:
                                for i in range(300):
                                    directory = root / f'concurrent_before_{i}'
                                    directory.mkdir()
                                    (directory / 'concurrent_live.txt').write_text('live')
                                    moved = root / f'concurrent_after_{i}'
                                    directory.rename(moved)
                                    if i % 2 == 0:
                                        (moved / 'concurrent_live.txt').unlink()
                                        moved.rmdir()
                                    time.sleep(.002)
                            except BaseException as error:
                                writer_errors.append(error)
                        writer = threading.Thread(target=mutate_during_recovery)
                        writer.start()
                    process.send_signal(signal.SIGCONT)
                events.settimeout(15)
                deadline = time.monotonic() + 15
                while receive(events)['type'] != 'resync_required':
                    assert time.monotonic() < deadline, 'no resync after real queue overflow'
            assert not search('overflow_stale'), 'overflow recovery retained deletion'
            assert not search('overflow_churn'), 'overflow recovery retained transient files'
            assert len(search('overflow_survivor')) == 25, 'overflow recovery lost surviving files'
            with connect(sockets / 'search.sock') as status:
                send(status, {'command': 'status'})
                assert receive(status)['fanotify_overflows'] >= 1, 'fixture did not overflow kernel queue'
            if writer:
                writer.join(timeout=10)
                assert not writer.is_alive() and not writer_errors, writer_errors
            deadline = time.monotonic() + 10
            expected = {str(item).encode() for item in root.rglob('*')}
            while True:
                with sqlite3.connect(sockets / 'catalog.db') as database:
                    durable = {bytes(row[0]) for row in database.execute('SELECT path FROM entries')}
                if durable == expected:
                    break
                assert time.monotonic() < deadline, f'durable catalog differs after overflow: missing={len(expected-durable)} {list(expected-durable)[:3]}, extra={len(durable-expected)} {list(durable-expected)[:3]}'
                time.sleep(.02)
            if writer:
                with connect(sockets / 'search.sock') as sock:
                    send(sock, {'query': 'concurrent_live', 'max_results': 1000})
                    assert len(receive(sock)['results']) == 150, 'concurrent survivors not searchable'
        with connect(sockets / 'events.sock') as transient_events:
            send(transient_events, {'command': 'subscribe_changes', 'version': 1,
                'scope_path': str(root), 'actions': ['create', 'rename', 'delete']})
            assert receive(transient_events)['type'] == 'subscribed'
            process.send_signal(signal.SIGSTOP)
            try:
                transient = root / 'transient_before.txt'
                transient.write_text('short lived')
                transient_moved = root / 'transient_after.txt'
                transient.rename(transient_moved)
                transient_moved.unlink()
                transient_dir = root / 'transient_dir_before'
                transient_dir.mkdir()
                (transient_dir / 'transient_child.txt').write_text('short lived subtree')
                moved_dir = root / 'transient_dir_after'
                transient_dir.rename(moved_dir)
                (moved_dir / 'transient_child.txt').unlink()
                moved_dir.rmdir()
            finally:
                process.send_signal(signal.SIGCONT)
            seen = []
            try:
                for _ in range(8):
                    seen.append(receive(transient_events))
            except TimeoutError:
                raise AssertionError(f'missing transient events: {seen}')
            file_events = [event for event in seen if event.get('name') in {'transient_before.txt', 'transient_after.txt'}]
            assert [event['action'] for event in file_events] == ['create', 'rename', 'delete'], file_events
            assert len({event['id'] for event in file_events}) == 1 and file_events[0]['id'], 'transient file lost identity'
            directory_events = [event for event in seen if event.get('is_directory')]
            assert [event['action'] for event in directory_events] == ['create', 'rename', 'delete'], directory_events
            child_events = [event for event in seen if event.get('name') == 'transient_child.txt']
            assert [event['action'] for event in child_events] == ['create', 'delete'], child_events
            assert child_events[1]['path'] in {str(moved_dir / 'transient_child.txt'), str(transient_dir / 'transient_child.txt')}, 'transient child deletion path invalid'
            assert child_events[0]['id'] == child_events[1]['id'], 'merged transient child lost identity'
            assert not search('transient_'), 'transient files remain indexed'
            process.send_signal(signal.SIGSTOP)
            try:
                coalesced = root / 'coalesced_directory'
                coalesced.mkdir()
                (coalesced / 'coalesced_child.txt').write_text('coalesced subtree')
                (coalesced / 'coalesced_child.txt').unlink()
                coalesced.rmdir()
            finally:
                process.send_signal(signal.SIGCONT)
            coalesced_events = [receive(transient_events) for _ in range(4)]
            for name in ('coalesced_directory', 'coalesced_child.txt'):
                matching = [event for event in coalesced_events if event['name'] == name]
                assert [event['action'] for event in matching] == ['create', 'delete'], matching
                assert len({event['id'] for event in matching}) == 1, 'coalesced identity changed'
            assert not search('coalesced_'), 'coalesced subtree remains indexed'
        identity = root / 'identity_original.txt'
        link = root / 'identity_hardlink.txt'
        identity.write_text('identity')
        os.link(identity, link)
        deadline = time.monotonic() + 5
        while len(search('identity_')) != 2:
            assert time.monotonic() < deadline
            time.sleep(.02)
        ids = {result['name']: result['id'] for result in search('identity_')}
        assert ids['identity_original.txt'] != ids['identity_hardlink.txt'] and all(ids.values()), 'hardlinks share entry ID'
        identity_renamed = root / 'identity_renamed.txt'
        with connect(sockets / 'events.sock') as identity_events:
            send(identity_events, {'command': 'subscribe_changes', 'version': 1, 'actions': ['rename']})
            assert receive(identity_events)['type'] == 'subscribed'
            identity.rename(identity_renamed)
            renamed_event = receive(identity_events)
            assert renamed_event['id'] == ids['identity_original.txt'], 'rename changed entry ID'
        assert search('identity_renamed')[0]['id'] == ids['identity_original.txt'], 'search ID differs from event ID'
        recycled = root / 'identity_recycled.txt'
        recycled.write_text('generation mismatch fixture')
        deadline = time.monotonic() + 5
        while not search('identity_recycled'):
            assert time.monotonic() < deadline
            time.sleep(.02)
        recycled_id = search('identity_recycled')[0]['id']
        raw_paths = [os.fsencode(root) + b'/raw_filename_' + bytes([byte]) + b'.txt' for byte in (0xfe, 0xff)]
        with connect(sockets / 'events.sock') as raw_events:
            send(raw_events, {'command': 'subscribe_changes', 'version': 1})
            assert receive(raw_events)['type'] == 'subscribed'
            for path in raw_paths:
                with open(path, 'wb') as output:
                    output.write(b'raw bytes')
            deadline = time.monotonic() + 5
            while len(search('raw_filename')) != 2:
                assert time.monotonic() < deadline, 'raw filenames not searchable'
                time.sleep(.02)
            results = search('raw_filename')
            assert {bytes.fromhex(item['path_bytes_hex']) for item in results} == set(raw_paths), 'raw filename identities collapsed'
            renamed = os.fsencode(root) + b'/raw_renamed_\xff.txt'
            os.rename(raw_paths[1], renamed)
            while True:
                event = receive(raw_events)
                if event.get('action') == 'rename' and event.get('path_bytes_hex') == renamed.hex():
                    assert bytes.fromhex(event['old_path_bytes_hex']) == raw_paths[1], 'raw rename source corrupted'
                    break
            os.unlink(raw_paths[0])
            os.unlink(renamed)
            deadline = time.monotonic() + 5
            while search('raw_filename') or search('raw_renamed'):
                assert time.monotonic() < deadline, 'raw filename deletion missed'
                time.sleep(.02)
        raw_directory = os.fsencode(root) + b'/raw_scope_\xff'
        os.mkdir(raw_directory)
        raw_child = raw_directory + b'/scope_child.txt'
        with connect(sockets / 'events.sock') as scoped_raw:
            send(scoped_raw, {'command': 'subscribe_changes', 'version': 1,
                'scope_path_bytes_hex': raw_directory.hex(), 'actions': ['create', 'delete'],
                'include_directories': False})
            assert receive(scoped_raw)['type'] == 'subscribed'
            with open(raw_child, 'wb') as output:
                output.write(b'raw scope')
            event = receive(scoped_raw)
            assert bytes.fromhex(event['path_bytes_hex']) == raw_child, 'raw subscription scope missed child'
            with connect(sockets / 'search.sock') as scoped_search:
                send(scoped_search, {'query': 'scope_child', 'scope_path_bytes_hex': (raw_directory + b'/').hex()})
                response = receive(scoped_search)
                assert response['ok'] and len(response['results']) == 1, response
                assert bytes.fromhex(response['results'][0]['path_bytes_hex']) == raw_child
            os.unlink(raw_child)
            assert receive(scoped_raw)['action'] == 'delete'
        os.rmdir(raw_directory)
        for invalid_scope in ('0', 'gg', '2f00', '72656c6174697665'):
            with connect(sockets / 'search.sock') as invalid:
                send(invalid, {'query': 'scope', 'scope_path_bytes_hex': invalid_scope})
                assert not receive(invalid)['ok'], 'invalid byte scope accepted'
        duplicate = subprocess.run([sys.argv[1], str(root), str(sockets)], capture_output=True, timeout=5)
        assert duplicate.returncode != 0 and b'another service' in duplicate.stderr, duplicate.stderr
        assert search('filtered'), 'duplicate instance damaged running service'
        with connect(sockets / 'search.sock') as status:
            send(status, {'command': 'status'})
            old_position = receive(status)['position']
        process.kill()
        process.wait(timeout=5)
        # Simulate a stale generation while retaining the exact cached path,
        # device and inode: inode-only matching would incorrectly keep the ID.
        with sqlite3.connect(sockets / 'catalog.db') as database:
            database.execute('UPDATE entries SET object_key=? WHERE path=?', (b'stale-generation', str(recycled).encode()))
        database.close()
        if '--corrupt-cache' in sys.argv:
            # All connections and the service are closed. Remove only this
            # fixture's sidecars so SQLite cannot repair the corrupt main DB
            # from a valid WAL (a malformed WAL can instead produce IOERR).
            for suffix in ('-wal', '-shm'):
                (sockets / ('catalog.db' + suffix)).unlink(missing_ok=True)
            (sockets / 'catalog.db').write_bytes(b'deliberately corrupt derived catalog')
        assert (sockets / 'search.sock').exists(), 'crash fixture did not leave stale socket'
        (root / 'offline_change.txt').write_text('while stopped')
        process = subprocess.Popen([sys.argv[1], str(root), str(sockets)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 5
        while True:
            assert process.poll() is None, process.stderr.read().decode() if process.poll() is not None else ''
            try:
                if search('offline_change'):
                    break
            except (ConnectionError, FileNotFoundError):
                pass
            assert time.monotonic() < deadline, 'restart failed to become ready'
            time.sleep(.02)
        with connect(sockets / 'events.sock') as resumed:
            send(resumed, {'command': 'subscribe_changes', 'version': 1, 'since': old_position})
            assert receive(resumed)['type'] == 'subscribed'
            assert receive(resumed)['type'] == 'resync_required', 'restart accepted stale cursor'
        if '--corrupt-cache' not in sys.argv:
            assert search('identity_renamed')[0]['id'] == ids['identity_original.txt'], 'restart changed surviving entry ID'
            assert search('identity_hardlink')[0]['id'] == ids['identity_hardlink.txt'], 'restart merged hardlinks'
            assert search('identity_recycled')[0]['id'] != recycled_id, 'generation mismatch retained old ID'
            assert search('offline_change')[0]['id'] not in ids.values(), 'restart reused allocated ID'
        else:
            assert search('identity_renamed') and search('identity_hardlink'), 'corrupt recovery lost live entries'
            with connect(sockets / 'search.sock') as status:
                send(status, {'command': 'status'})
                quarantine = pathlib.Path(receive(status)['recovered_database'])
                assert quarantine.parent == sockets and (quarantine / 'catalog.db').read_bytes() == b'deliberately corrupt derived catalog', f'corrupt cache not preserved: quarantine={quarantine}'
        with sqlite3.connect(sockets / 'catalog.db') as database:
            assert database.execute('PRAGMA integrity_check').fetchone() == ('ok',), 'crash damaged database'
            assert database.execute('SELECT path FROM entries WHERE path=?', (str(root / 'offline_change.txt').encode(),)).fetchone(), 'restart reconciliation not durable'
            assert not any(bytes(row[0]).startswith(str(sockets).encode() + b'/') for row in database.execute('SELECT path FROM entries')), 'state directory leaked into durable catalog'
        print('linux_service_e2e passed')
    finally:
        if writer:
            writer.join(timeout=10)
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError('service failed to shut down')
        resources.close()
        assert process.returncode == 0, process.stderr.read().decode()

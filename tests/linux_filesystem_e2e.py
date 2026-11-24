"""Real XFS/Btrfs service search, snapshots and notifications in private mounts."""
import json
import os
import pathlib
import shutil
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time


def send(peer, request):
    body = json.dumps(request).encode()
    peer.sendall(struct.pack('<I', len(body)) + body)


def receive(peer):
    def exact(count):
        data = b''
        while len(data) < count:
            chunk = peer.recv(count - len(data))
            assert chunk, 'incomplete frame'
            data += chunk
        return data
    size, = struct.unpack('<I', exact(4))
    assert 0 < size <= 1024 * 1024
    return json.loads(exact(size))


def connect(endpoint):
    peer = socket.socket(socket.AF_UNIX)
    peer.settimeout(6)
    peer.connect(str(endpoint))
    return peer


filesystem = sys.argv[2]
assert filesystem in ('xfs', 'btrfs')
if os.geteuid() != 0 or not shutil.which('mkfs.' + filesystem):
    sys.exit(77)
if filesystem not in pathlib.Path('/proc/filesystems').read_text().split():
    print(f'{filesystem} kernel support must be loaded before testing')
    sys.exit(77)
subprocess.run(['mount', '--make-rprivate', '/'], check=True)
with tempfile.TemporaryDirectory(prefix=f'filesearch-{filesystem}-', dir='/home/donggua') as temporary:
    base = pathlib.Path(temporary)
    image, disk, ipc = base / 'owned.image', base / 'disk', base / 'ipc'
    with image.open('wb') as output:
        output.truncate(512 * 1024 * 1024)
    disk.mkdir(); ipc.mkdir()
    command = ['mkfs.' + filesystem, '-f', '-q']
    if filesystem == 'xfs':
        command += ['-m', 'crc=1,reflink=1,bigtime=0,rmapbt=0', '-i', 'nrext64=0']
    subprocess.run(command + [str(image)], check=True)
    subprocess.run(['mount', '-o', 'loop', str(image), str(disk)], check=True)
    process = None
    try:
        root = disk / 'files'
        root.mkdir()
        baseline = root / '中文.txt'
        baseline.write_text('pinyin')
        os.link(baseline, root / 'hardlink.txt')
        directory = root / 'before'
        directory.mkdir()
        (directory / 'child.txt').write_text('child')
        if filesystem == 'btrfs':
            for name in ('subvolume_a', 'subvolume_b'):
                subprocess.run(['btrfs', 'subvolume', 'create', str(root / name)], check=True, stdout=subprocess.DEVNULL)
                (root / name / 'shared_inode.txt').write_text(name)
        process = subprocess.Popen([sys.argv[1], str(root), str(ipc)], stderr=subprocess.PIPE)
        search_endpoint = ipc / 'search.sock'
        deadline = time.monotonic() + 8
        while not search_endpoint.exists():
            assert process.poll() is None, process.stderr.read().decode()
            assert time.monotonic() < deadline, 'startup timeout'
            time.sleep(.02)

        while True:
            with connect(search_endpoint) as peer:
                send(peer, {'command': 'status'})
                if receive(peer)['ready']:
                    break
            assert process.poll() is None and time.monotonic() < deadline, 'validation timeout'
            time.sleep(.02)

        def search(query):
            with connect(search_endpoint) as peer:
                send(peer, {'query': query})
                response = receive(peer)
                assert response['ok'], response
                return response['results']

        chinese, hardlink = search('zhongwen'), search('hardlink')
        assert len(chinese) == 1 and len(hardlink) == 1
        assert baseline.stat().st_ino == (root / 'hardlink.txt').stat().st_ino
        assert chinese[0]['id'] != hardlink[0]['id']
        old_child_id = search('child.txt')[0]['id']
        if filesystem == 'btrfs':
            same_inode = search('shared_inode')
            assert len(same_inode) == 2 and same_inode[0]['id'] != same_inode[1]['id']
            assert (root / 'subvolume_a' / 'shared_inode.txt').stat().st_ino == (root / 'subvolume_b' / 'shared_inode.txt').stat().st_ino
        with connect(ipc / 'events.sock') as events:
            send(events, {'command': 'subscribe_changes', 'actions': ['create', 'rename', 'delete']})
            assert receive(events)['type'] == 'subscribed'
            live = root / 'live.txt'
            live.write_text('live')
            created = receive(events)
            assert created['action'] == 'create' and created['path'] == str(live), created
            assert search('live.txt')[0]['id'] == created['id']
            moved = root / 'moved.txt'
            live.rename(moved)
            renamed = receive(events)
            assert renamed['action'] == 'rename' and renamed['id'] == created['id'], renamed
            assert search('moved.txt')[0]['id'] == created['id']
            moved.unlink()
            assert receive(events)['action'] == 'delete'
            assert not search('moved.txt')
            directory.rename(root / 'after')
            renamed = receive(events)
            assert renamed['action'] == 'rename' and renamed['is_directory'], renamed
            child = search('child.txt')[0]
            assert child['id'] == old_child_id and child['path'] == str(root / 'after' / 'child.txt')
            raw = os.fsencode(root) + b'/raw_\xff.txt'
            with open(raw, 'wb') as output:
                output.write(b'raw')
            created = receive(events)
            assert bytes.fromhex(created['path_bytes_hex']) == raw, created
            assert bytes.fromhex(search('raw_')[0]['path_bytes_hex']) == raw
            if filesystem == 'btrfs':
                nested_live = root / 'subvolume_a' / 'subvolume_live.txt'
                nested_live.write_text('subvolume live')
                created = receive(events)
                assert created['action'] == 'create' and created['path'] == str(nested_live), created
                assert search('subvolume_live')[0]['id'] == created['id']
                nested_live.unlink()
                assert receive(events)['action'] == 'delete'
                dynamic = root / 'dynamic_subvolume'
                subprocess.run(['btrfs', 'subvolume', 'create', str(dynamic)], check=True, stdout=subprocess.DEVNULL)
                created = receive(events)
                assert created['action'] == 'create' and created['is_directory'] and created['path'] == str(dynamic), created
                (dynamic / 'dynamic_child.txt').write_text('live')
                created = receive(events)
                assert created['action'] == 'create' and created['path'] == str(dynamic / 'dynamic_child.txt'), created
                assert search('dynamic_child')[0]['id'] == created['id']
                cloned = root / 'readonly_snapshot'
                subprocess.run(['btrfs', 'subvolume', 'snapshot', '-r', str(root / 'subvolume_a'), str(cloned)],
                    check=True, stdout=subprocess.DEVNULL)
                while True:
                    created = receive(events)
                    if created['action'] == 'create' and created['path'] == str(cloned):
                        assert created['is_directory']
                        break
                copies = search('shared_inode')
                assert len(copies) == 3 and len({entry['id'] for entry in copies}) == 3
                assert any(entry['path'] == str(cloned / 'shared_inode.txt') for entry in copies)
                subprocess.run(['btrfs', 'subvolume', 'delete', str(cloned)], check=True, stdout=subprocess.DEVNULL)
                while True:
                    removed = receive(events)
                    if removed['action'] == 'delete' and removed['path'] == str(cloned):
                        break
                assert len(search('shared_inode')) == 2
        expected = {os.fsencode(entry) for entry in root.rglob('*')}
        captured = []
        with connect(search_endpoint) as peer:
            send(peer, {'command': 'snapshot', 'page_size': 1})
            position = None
            while True:
                page = receive(peer)
                assert page['type'] == 'snapshot_page', page
                if position is None:
                    position = page['position']
                assert page['position'] == position
                captured.extend(page['entries'])
                if page['done']:
                    break
                send(peer, {'command': 'snapshot_next'})
        assert {bytes.fromhex(entry['path_bytes_hex']) for entry in captured} == expected
        with sqlite3.connect(ipc / 'catalog.db') as database:
            assert database.execute('PRAGMA integrity_check').fetchone() == ('ok',)
            assert {bytes(row[0]) for row in database.execute('SELECT path FROM entries')} == expected
        if filesystem == 'btrfs':
            # A configured root inside a subvolume needs a top-level mark
            # fallback, not merely normalization for descendants of root 5.
            for mode in ('nested-root', 'mounted-subvolume-root'):
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=5)
                assert process.returncode == 0, process.stderr.read().decode()
                if mode == 'mounted-subvolume-root':
                    subprocess.run(['umount', str(disk)], check=True)
                    subprocess.run(['mount', '-o', 'loop,subvol=/files/subvolume_a', str(image), str(disk)], check=True)
                    configured = disk
                else:
                    configured = root / 'subvolume_a'
                process = subprocess.Popen([sys.argv[1], str(configured), str(ipc)], stderr=subprocess.PIPE)
                deadline = time.monotonic() + 8
                while not search_endpoint.exists():
                    assert process.poll() is None, process.stderr.read().decode()
                    assert time.monotonic() < deadline, f'{mode} startup timeout'
                    time.sleep(.02)
                assert len(search('shared_inode')) == 1, mode
                with connect(ipc / 'events.sock') as events:
                    send(events, {'command': 'subscribe_changes', 'actions': ['create', 'delete']})
                    assert receive(events)['type'] == 'subscribed'
                    probe = configured / f'{mode}_live.txt'
                    probe.write_text(mode)
                    created = receive(events)
                    assert created['action'] == 'create' and created['path'] == str(probe), (mode, created)
                    assert search(probe.name)[0]['id'] == created['id'], mode
                    probe.unlink()
                    assert receive(events)['action'] == 'delete'
        print(f'linux_filesystem_e2e passed ({filesystem})')
    finally:
        try:
            if process is not None:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
                if sys.exc_info()[0] is None:
                    assert process.returncode == 0, process.stderr.read().decode()
        finally:
            subprocess.run(['umount', str(disk)], check=True)

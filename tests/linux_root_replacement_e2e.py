"""Configured root lifetime: move-out, absence, symlink, recreation and push."""
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


def send(peer, request):
    body = json.dumps(request).encode()
    peer.sendall(struct.pack('<I', len(body)) + body)


def receive(peer):
    def exact(count):
        data = b''
        while len(data) < count:
            chunk = peer.recv(count - len(data))
            assert chunk, 'unexpected connection close'
            data += chunk
        return data
    size, = struct.unpack('<I', exact(4))
    assert 0 < size <= 1024 * 1024
    return json.loads(exact(size))


def request(endpoint, value):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(5)
        peer.connect(str(endpoint))
        send(peer, value)
        return receive(peer)


if os.geteuid() != 0:
    sys.exit(77)
with tempfile.TemporaryDirectory(prefix='filesearch-root-', dir='/home/donggua') as temporary:
    base = pathlib.Path(temporary)
    root, ipc = base / 'files', base / 'ipc'
    root.mkdir()
    (root / 'old_generation.txt').write_text('old')
    ipc.mkdir()
    process = subprocess.Popen([sys.argv[1], str(root), str(ipc)], stderr=subprocess.PIPE)
    endpoint = ipc / 'search.sock'

    def wait_for(predicate):
        end = time.monotonic() + 8
        while True:
            assert process.poll() is None, process.stderr.read().decode()
            if endpoint.exists():
                status = request(endpoint, {'command': 'status'})
                if predicate(status):
                    return status
            assert time.monotonic() < end, 'root recovery timed out'
            time.sleep(.03)

    try:
        initial = wait_for(lambda status: status['ready'])
        assert request(endpoint, {'query': 'old_generation'})['results']
        root.rename(base / 'detached')
        missing = wait_for(lambda status: not status['ready'])
        assert missing['position']['epoch'] != initial['position']['epoch']
        unavailable = request(endpoint, {'query': 'old_generation'})
        assert not unavailable['ok'] and unavailable['code'] == 'not_ready', unavailable
        # A symlink must never become a new indexed root.
        root.symlink_to(base / 'detached', target_is_directory=True)
        time.sleep(.4)
        assert not request(endpoint, {'command': 'status'})['ready']
        root.unlink()
        root.mkdir()
        (root / 'new_generation.txt').write_text('new')
        wait_for(lambda status: status['ready'])
        assert request(endpoint, {'query': 'new_generation'})['results']
        assert not request(endpoint, {'query': 'old_generation'})['results']
        with socket.socket(socket.AF_UNIX) as peer:
            peer.settimeout(5)
            peer.connect(str(ipc / 'events.sock'))
            send(peer, {'command': 'subscribe_changes', 'since': initial['position']})
            assert receive(peer)['type'] == 'subscribed'
            assert receive(peer)['type'] == 'resync_required'
        with socket.socket(socket.AF_UNIX) as peer:
            peer.settimeout(5)
            peer.connect(str(ipc / 'events.sock'))
            send(peer, {'command': 'subscribe_changes', 'actions': ['create']})
            assert receive(peer)['type'] == 'subscribed'
            live = root / 'live_after_root_recovery.txt'
            live.write_text('live')
            event = receive(peer)
            assert event['action'] == 'create' and event['path'] == str(live), event
            assert request(endpoint, {'query': 'live_after_root_recovery'})['results']
        # Also cover deletion (rather than move-out) of the configured root.
        for child in root.iterdir():
            child.unlink()
        root.rmdir()
        wait_for(lambda status: not status['ready'])
        root.mkdir()
        (root / 'third_generation.txt').write_text('third')
        wait_for(lambda status: status['ready'])
        assert request(endpoint, {'query': 'third_generation'})['results']
        assert not request(endpoint, {'query': 'new_generation'})['results']
        print('linux_root_replacement_e2e passed')
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError('service shutdown timed out')
        assert process.returncode == 0, process.stderr.read().decode()

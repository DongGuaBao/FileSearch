"""Opt-in live systemd test using an isolated transient unit, not production."""
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid


def command(*arguments):
    return subprocess.check_output(arguments, text=True).strip()


def request(state, body):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(5)
        peer.connect(str(state / 'search.sock'))
        encoded = json.dumps(body).encode()
        peer.sendall(struct.pack('<I', len(encoded)) + encoded)
        def exact(length):
            result = b''
            while len(result) < length:
                chunk = peer.recv(length - len(result))
                assert chunk
                result += chunk
            return result
        length, = struct.unpack('<I', exact(4))
        assert 0 < length <= 1024 * 1024
        return json.loads(exact(length))


if os.geteuid() != 0:
    sys.exit(77)
name = 'filesearch-test-' + uuid.uuid4().hex
unit = name + '.service'
state = pathlib.Path('/var/lib') / name
assert not state.exists()
with tempfile.TemporaryDirectory(prefix='filesearch-systemd-', dir='/home/donggua') as temporary:
    root = pathlib.Path(temporary)
    (root / 'systemd_original.txt').write_text('fixture')
    started = False
    try:
        # Mirror deploy/filesearch-linux.service's runtime settings with an
        # isolated StateDirectory and transient unit, leaving installed units alone.
        command('systemd-run', '--unit=' + name, '--property=Type=simple',
                '--property=User=root', '--property=Group=root', '--property=UMask=0077',
                '--property=StateDirectory=' + name, '--property=StateDirectoryMode=0700',
                '--property=Restart=on-failure', '--property=RestartSec=3',
                '--property=TimeoutStopSec=30', str(pathlib.Path(sys.argv[1]).resolve()), '/', str(state))
        started = True
        def ready(old_epoch=None):
            deadline = time.monotonic() + 120
            while True:
                try:
                    status = request(state, {'command': 'status'})
                    if status['ready'] and (old_epoch is None or status['position']['epoch'] != old_epoch):
                        return status
                except (FileNotFoundError, ConnectionError, OSError):
                    pass
                assert time.monotonic() < deadline, command('systemctl', 'show', unit, '-p', 'ActiveState', '-p', 'SubState')
                time.sleep(.1)
        status = ready()
        before = request(state, {'query': 'systemd_original', 'scope_path': str(root)})['results']
        assert len(before) == 1
        identity = before[0]['id']
        assert state.stat().st_mode & 0o777 == 0o700
        assert (state / 'search.sock').stat().st_mode & 0o777 == 0o600
        pid = int(command('systemctl', 'show', unit, '-p', 'MainPID', '--value'))
        os.kill(pid, 9)
        (root / 'systemd_offline.txt').write_text('created while stopped')
        restarted = ready(status['position']['epoch'])
        assert int(command('systemctl', 'show', unit, '-p', 'MainPID', '--value')) != pid
        assert request(state, {'query': 'systemd_original', 'scope_path': str(root)})['results'][0]['id'] == identity
        assert len(request(state, {'query': 'systemd_offline', 'scope_path': str(root)})['results']) == 1
        assert int(command('systemctl', 'show', unit, '-p', 'NRestarts', '--value')) >= 1
        command('systemctl', 'stop', unit)
        started = False # Successful transient units are unloaded after stop.
        assert not (state / 'search.sock').exists() and not (state / 'events.sock').exists()
        assert (state / 'catalog.db').exists(), 'normal stop lost durable state'
        print('linux_systemd_e2e passed (root scope, automatic restart, durable IDs, offline recovery, graceful stop)')
    finally:
        if started:
            subprocess.run(['systemctl', 'stop', unit], check=True)
            subprocess.run(['systemctl', 'reset-failed', unit], check=False, stderr=subprocess.DEVNULL)
        assert state.resolve() == pathlib.Path('/var/lib') / name and name.startswith('filesearch-test-')
        if state.exists():
            shutil.rmtree(state)

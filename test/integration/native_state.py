#!/usr/bin/env python3
import hashlib
import os
import pathlib
import subprocess
import sys
import time
import uuid

IMAGE = sys.argv[1]
BINARY = None if sys.argv[2] == '--image-native' else pathlib.Path(sys.argv[2]).resolve()
LAB = pathlib.Path(__file__).resolve().parent
RPC = LAB / 'native-rpc.php'
NAME = 'rem4-nativefix-state-' + uuid.uuid4().hex[:10]

def docker(*args, check=True):
    return subprocess.run(['docker', *args], text=True, capture_output=True, check=check)

def inside(*args, check=True):
    return docker('exec', '--user', '991:991', NAME, *args, check=check)

def rpc(method, *args, check=True):
    return inside('php85', '/rpc.php', method, *args, check=check)

def wait(predicate, description, timeout=30):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate(): return
        if docker('inspect', '-f', '{{.State.Running}}', NAME).stdout.strip() != 'true':
            raise AssertionError('daemon exited while ' + description)
        time.sleep(.15)
    raise AssertionError('timeout: ' + description)

def write(path, body):
    inside('php85', '-r', 'file_put_contents($argv[1], base64_decode($argv[2])); chmod($argv[1],0600);', path,
           __import__('base64').b64encode(body).decode())

def fixture(name):
    info = b'd6:lengthi8e4:name' + str(len(name)).encode() + b':' + name.encode() + b'12:piece lengthi16384e6:pieces20:' + hashlib.sha1(b'xxxxxxxx').digest() + b'e'
    path = '/data/' + name + '.torrent'
    write(path, b'd4:info' + info + b'e')
    return path, hashlib.sha1(info).hexdigest().upper(), info

try:
    docker('run', '-d', '--name', NAME, '--stop-timeout', '60', '--network', 'none',
           '-e', 'RR_MIG_TEST_CUT=after-original-claim',
           '-e', 'TEST_SESSION_MODE=' + os.environ.get('TEST_SESSION_MODE', '2755'),
           *(['--mount', f'type=bind,src={BINARY},dst=/native-bin,readonly'] if BINARY else []),
           '--mount', f'type=bind,src={LAB},dst=/lab,readonly',
           '--mount', f'type=bind,src={RPC},dst=/rpc.php,readonly',
           '--entrypoint', '/bin/sh', IMAGE, '/lab/boot-native.sh')
    wait(lambda: rpc('system.pid', check=False).returncode == 0, 'SCGI readiness')
    path, hash_, readd_info = fixture('native-readd')
    rpc('load.start', '', path)
    wait(lambda: rpc('d.state', hash_, check=False).stdout.strip() == '1', 'torrent start')
    rpc('d.stop', hash_)
    rpc('d.close', hash_)
    assert inside('stat', '-c', '%s', '/data/.session/.rutorrent-stop-intents/' + hash_).stdout.strip() == '17'
    rpc('d.start', hash_)
    rpc('d.erase', hash_)
    assert inside('test', '-e', '/data/.session/.rutorrent-stop-intents/' + hash_, check=False).returncode != 0
    path = '/data/native-readd-fresh.torrent'
    write(path, b'd4:info' + readd_info + b'e')
    write('/data/.session/.rutorrent-stop-intents/' + hash_, b'0000000000000008\n')
    rpc('load.start', '', path)
    wait(lambda: rpc('d.state', hash_, check=False).stdout.strip() == '1', 'same-hash reload')
    rpc('session.save')
    orphan = 'B' * 40
    write('/data/.session/.rutorrent-stop-intents/' + orphan, b'0000000000000001\n')
    write('/data/.session/.rutorrent-stop-intents/' + hash_ + '.claim.' + 'a' * 32 + '.tmp', b'partial')
    docker('stop', NAME)
    assert docker('inspect', '-f', '{{.State.ExitCode}}', NAME).stdout.strip() == '0'
    docker('start', NAME)
    wait(lambda: rpc('system.pid', check=False).returncode == 0, 'restart readiness')
    assert rpc('d.state', hash_).stdout.strip() == '1'
    assert inside('test', '-e', '/data/.session/.rutorrent-stop-intents/' + orphan, check=False).returncode != 0
    assert inside('test', '-e', '/data/.session/.rutorrent-stop-intents/' + hash_ + '.claim.' + 'a' * 32 + '.tmp', check=False).returncode != 0
    print('ok - erase retires counter, new same-hash load baselines legacy generation, restart preserves running state, orphan/temp cleanup')
    _, meta_hash, info = fixture('native-meta')
    write('/data/native-info', info)
    rpc('load.start', '', 'magnet:?xt=urn:btih:' + meta_hash)
    wait(lambda: rpc('d.is_meta', meta_hash, check=False).stdout.strip() == '1', 'magnet placeholder')
    local_id = rpc('d.local_id', meta_hash).stdout.strip()
    refusal = rpc('d.retrackers.begin_original', meta_hash, local_id, '0' * 64, '1', check=False)
    assert refusal.returncode != 0 and 'requires resolved torrent metadata' in refusal.stderr
    assert inside('test', '-e', '/data/.session/.retrackers-migration/' + meta_hash + '.preclaim.receipt', check=False).returncode != 0
    docker('exec', '-d', '--user', '991:991', NAME, '/bin/sh', '-c', 'php85 /lab/meta-peer.php >/tmp/meta-peer.log 2>&1')
    wait(lambda: inside('test', '-f', '/data/meta-peer-ready', check=False).returncode == 0, 'local metadata peer')
    rpc('add_peer', meta_hash, '127.0.0.1:45552')
    wait(lambda: rpc('d.is_meta', meta_hash, check=False).stdout.strip() == '0', 'resolved magnet conversion')
    assert rpc('d.state', meta_hash).stdout.strip() == '1'
    assert inside('test', '-e', '/data/.session/.rutorrent-stop-intents/' + meta_hash, check=False).returncode != 0
    rpc('session.save')
    docker('stop', NAME)
    docker('start', NAME)
    wait(lambda: rpc('system.pid', check=False).returncode == 0, 'magnet restart readiness')
    assert rpc('d.state', meta_hash).stdout.strip() == '1'
    print('ok - meta claim rejected without receipt, local metadata resolves without daemon crash or stop journal, running state survives restart')
    for index, stop_event in enumerate(('event.download.closed', 'event.download.erased')):
        name = 'native-meta-stop-' + str(index)
        _, stopped_hash, stopped_info = fixture(name)
        write('/data/' + name + '-info', stopped_info)
        rpc('load.start', '', 'magnet:?xt=urn:btih:' + stopped_hash)
        wait(lambda: rpc('d.is_meta', stopped_hash, check=False).stdout.strip() == '1', name + ' placeholder')
        rpc('method.set_key', '', stop_event, 'rem4-meta-stop', 'd.stop=')
        docker('exec', '-d', '--user', '991:991', NAME, 'php85', '/lab/meta-peer.php',
               '/data/' + name + '-info', str(45553 + index), name)
        wait(lambda: inside('test', '-f', '/data/' + name + '-ready', check=False).returncode == 0, name + ' peer')
        rpc('add_peer', stopped_hash, '127.0.0.1:' + str(45553 + index))
        wait(lambda: rpc('d.is_meta', stopped_hash, check=False).stdout.strip() == '0', name + ' conversion')
        rpc('method.set_key', '', stop_event, 'rem4-meta-stop')
        assert rpc('d.state', stopped_hash).stdout.strip() == '0'
        rpc('session.save')
        docker('stop', NAME)
        docker('start', NAME)
        wait(lambda: rpc('system.pid', check=False).returncode == 0, name + ' restart')
        assert rpc('d.state', stopped_hash).stdout.strip() == '0'
    print('ok - explicit Stop from close and erase metadata callbacks transfers to replacement and survives restart')
    local_id = rpc('d.local_id', hash_).stdout.strip()
    result = rpc('d.retrackers.begin_original', hash_, local_id, '0' * 64, '1')
    assert result.stdout.strip()
    assert rpc('system.pid').returncode == 0
    print('ok - production build ignores destructive RR_MIG_TEST_CUT environment')
except Exception:
    if hasattr(sys.exc_info()[1], 'stderr'): print(sys.exc_info()[1].stderr, file=sys.stderr)
    logs = docker('logs', NAME, check=False)
    print(logs.stdout + logs.stderr, file=sys.stderr)
    print(rpc('d.state', hash_, check=False), file=sys.stderr)
    print(inside('ls', '-lR', '/data', check=False).stdout, file=sys.stderr)
    print(inside('cat', '/tmp/native.log', check=False).stdout, file=sys.stderr)
    print(inside('cat', '/data/meta-peer-sent', check=False).stdout, file=sys.stderr)
    print(inside('cat', '/tmp/meta-peer.log', check=False).stdout, file=sys.stderr)
    raise
finally:
    docker('rm', '-f', '-v', NAME, check=False)

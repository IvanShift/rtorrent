#!/usr/bin/env python3
"""Run native review regressions against exact locally built daemon bytes.

Usage: native_regressions.py BUILDER_IMAGE BUILD_TREE SCRATCH [CASE ...]
The image must supply the matching libtorrent ABI and a C++ compiler. Each case
runs in a disposable network-isolated container; no public trackers are used.
"""
import hashlib
import os
import pathlib
import socket
import subprocess
import sys
import time
import uuid
import xmlrpc.client


def encode(value):
    if isinstance(value, int):
        return b'i' + str(value).encode() + b'e'
    if isinstance(value, (str, bytes)):
        value = value.encode() if isinstance(value, str) else value
        return str(len(value)).encode() + b':' + value
    if isinstance(value, list):
        return b'l' + b''.join(map(encode, value)) + b'e'
    return b'd' + b''.join(encode(k) + encode(v) for k, v in sorted(value.items())) + b'e'


def docker(*args, check=True):
    return subprocess.run(['docker', *args], check=check, capture_output=True, text=True, timeout=65)


class Daemon:
    def __init__(self, image, tree, scratch, case):
        self.image, self.tree = image, pathlib.Path(tree).resolve()
        self.name = 'rr-remediation-' + uuid.uuid4().hex[:12]
        self.cut = {'owner-cut-before': 'before-owner-ordinary',
                    'owner-cut-after': 'after-owner-ordinary',
                    'owner-cut-retry': 'before-owner-ordinary-throw'}.get(case, '')
        self.directory = pathlib.Path(scratch).resolve() / (case + '-' + uuid.uuid4().hex[:8])
        self.directory.mkdir(mode=0o755, parents=True)
        self.session = self.directory / '.session'
        self.session.mkdir(mode=0o2700)
        (self.directory / 'dl').mkdir()
        self.socket = self.directory / 'rtorrent.sock'
        (self.directory / 'test.rc').write_text('\n'.join([
            'system.daemon.set=true', 'session.path.set=/data/.session',
            'directory.default.set=/data/dl', 'network.scgi.open_local=/data/rtorrent.sock',
            'network.listen.port.range.set=45561-45561', 'network.block.ipv6.set=no',
            'dht.mode.set=disable', 'protocol.pex.set=no', 'trackers.use_udp.set=no',
            'log.open_file.flush="native",/data/native.log', 'log.add_output="error","native"',
            'log.add_output="torrent_error","native"', 'log.add_output="warn","native"', '']))

    def start(self):
        self.socket.unlink(missing_ok=True)
        (self.session / 'rtorrent.lock').unlink(missing_ok=True)
        docker('run', '-d', '--pull=never', '--name', self.name, '--network', 'none',
               '--stop-timeout', '60', '--user', f'{os.getuid()}:{os.getgid()}', '-e', 'HOME=/tmp',
               '-e', 'RR_MIG_TEST_CUT=' + self.cut,
               '-v', str(self.tree) + ':/w:ro', '-v', str(self.directory) + ':/data',
               '--entrypoint', '/w/src/rtorrent', self.image, '-n', '-o', 'import=/data/test.rc')
        self.wait(lambda: self.call('system.pid') > 0, 'SCGI readiness')

    def stop(self, kill=False):
        result = docker('kill' if kill else 'stop', *([] if kill else ['-t', '60']), self.name)
        if not kill:
            assert docker('inspect', '-f', '{{.State.ExitCode}}', self.name).stdout.strip() == '0', result
        docker('rm', self.name)

    def call(self, method, *args):
        body = xmlrpc.client.dumps(args, methodname=method).encode()
        headers = (b'CONTENT_LENGTH\0' + str(len(body)).encode() +
                   b'\0SCGI\0' + b'1\0REQUEST_METHOD\0POST\0REQUEST_URI\0/RPC2\0')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
            conn.settimeout(20)
            # Linux sun_path holds only 107 bytes; a relative socket name keeps
            # arbitrary workspace prefixes outside that limit.
            conn.connect(os.path.relpath(self.socket))
            conn.sendall(str(len(headers)).encode() + b':' + headers + b',' + body)
            parts = []
            while chunk := conn.recv(65536):
                parts.append(chunk)
        response = b''.join(parts)
        beginning = response.find(b'<?xml')
        if beginning < 0:
            raise RuntimeError('SCGI response lacks XML: ' + repr(response[:100]))
        return xmlrpc.client.loads(response[beginning:])[0][0]

    def wait(self, predicate, description, timeout=30):
        end = time.monotonic() + timeout
        last_error = ''
        while time.monotonic() < end:
            try:
                if predicate():
                    return
            except (OSError, xmlrpc.client.Fault, RuntimeError) as error:
                last_error = repr(error)
            time.sleep(.1)
        raise AssertionError('timeout: ' + description + '; last error: ' + last_error)

    def fixture(self, name, start=True):
        info = {'length': 8, 'name': name, 'piece length': 16384,
                'pieces': hashlib.sha1(b'xxxxxxxx').digest()}
        raw = encode({'info': info})
        hash_ = hashlib.sha1(encode(info)).hexdigest().upper()
        (self.directory / (name + '.torrent')).write_bytes(raw)
        self.call('load.start' if start else 'load.normal', '', '/data/' + name + '.torrent')
        self.wait(lambda: self.call('d.hash', hash_) == hash_, 'load ' + name)
        return hash_

    def begin(self, hash_):
        marker = self.call('d.retrackers.begin_original', hash_, self.call('d.local_id', hash_),
                           '0' * 64, str(self.call('d.state', hash_)))
        return marker.rsplit(':', 1)[1], self.call('d.incarnation', hash_)


def stop_cat(daemon):
    hash_ = daemon.fixture('stop-cat')
    result = daemon.call('d.multicall2', '', 'main', 'cat="$d.stop=",MARK')
    assert result == [['MARK']], result


def claim_restart(daemon):
    for operation in ('d.stop', 'd.pause'):
        hash_ = daemon.fixture('restart-' + operation.replace('.', '-'))
        daemon.begin(hash_)
        daemon.call(operation, hash_)
        daemon.call('session.save')
        daemon.call('d.start', hash_)
        daemon.call('session.save')
        assert daemon.call('d.state', hash_) == 1
        daemon.stop()
        daemon.start()
        assert daemon.call('d.state', hash_) == 1, operation + ': saved Start lost after restart'


def claim_finish(daemon):
    hash_ = daemon.fixture('finish-after-start')
    tx, incarnation = daemon.begin(hash_)
    daemon.call('d.stop', hash_)
    daemon.call('d.start', hash_)
    assert daemon.call('d.retrackers.finish_original', hash_, tx, incarnation) == 'finalized'
    assert daemon.call('d.state', hash_) == 1
    assert daemon.call('d.custom', hash_, 'retrackers-recovery') == ''
    daemon.call('d.erase', hash_)
    assert not daemon.call('d.multicall2', '', 'main', 'd.hash=')


def owner_finalize(daemon):
    hash_ = daemon.fixture('owner-finalize')
    tx, incarnation = daemon.begin(hash_)
    assert daemon.call('d.retrackers.finish_original', hash_, tx, incarnation) == 'finalized'
    pointer = daemon.session / '.retrackers-migration' / (hash_ + '.owner.current')
    assert pointer.read_text().startswith('ordinary:'), pointer.read_text()
    before = sorted((daemon.session / '.retrackers-migration').iterdir())
    daemon.call('d.custom.set', hash_, 'after-finalize', 'kept')
    daemon.call('session.save')
    assert sorted((daemon.session / '.retrackers-migration').iterdir()) == before
    daemon.stop()
    daemon.start()
    assert daemon.call('d.custom', hash_, 'after-finalize') == 'kept'
    assert pointer.read_text().startswith('ordinary:')
    daemon.call('d.erase', hash_)
    daemon.stop()
    daemon.start()
    assert not daemon.call('d.multicall2', '', 'main', 'd.hash='), 'retired owner resurrected'


def compile_peer(daemon):
    peer_source = pathlib.Path(__file__).with_name('metadata-peer.cc')
    docker('run', '--rm', '--pull=never', '--network', 'none',
           '--user', f'{os.getuid()}:{os.getgid()}', '-v', str(peer_source) + ':/peer.cc:ro',
           '-v', str(daemon.directory) + ':/data', '--entrypoint', '/bin/sh', daemon.image,
           '-c', 'g++ -std=c++20 -Wall -Wextra -Werror /peer.cc -lcrypto -o /data/metadata-peer')


def metadata_start(daemon):
    compile_peer(daemon)
    for index, (declared, stop_event) in enumerate(((0, None), (1, None),
                                                  (1, 'event.download.closed'),
                                                  (1, 'event.download.erased'))):
        name = 'metadata-' + str(index)
        info = {'length': 8, 'name': name, 'piece length': 16384,
                'pieces': hashlib.sha1(b'xxxxxxxx').digest()}
        hash_ = hashlib.sha1(encode(info)).hexdigest().upper()
        (daemon.directory / (name + '.info')).write_bytes(encode(info))
        daemon.call('load.start' if declared else 'load.normal', '', 'magnet:?xt=urn:btih:' + hash_)
        daemon.wait(lambda: daemon.call('d.is_meta', hash_) == 1, 'metadata placeholder')
        if not declared:
            daemon.call('d.start', hash_)
        if stop_event:
            daemon.call('method.set_key', '', stop_event, 'review-stop', 'd.stop=')
        port = 45562 + index
        docker('exec', '-d', daemon.name, '/data/metadata-peer', '/data/' + name + '.info',
               str(port), '/data/' + name + '.ready')
        daemon.wait(lambda: (daemon.directory / (name + '.ready')).exists(), 'local metadata peer')
        daemon.call('add_peer', hash_, '127.0.0.1:' + str(port))
        daemon.wait(lambda: daemon.call('d.is_meta', hash_) == 0, 'metadata conversion')
        if stop_event:
            daemon.call('method.set_key', '', stop_event, 'review-stop')
        expected = int(bool(declared and not stop_event))
        assert daemon.call('d.state', hash_) == expected, (declared, stop_event)
        if not expected:
            assert daemon.call('d.is_active', hash_) == 0
            assert not (daemon.directory / 'dl' / name).exists(), 'service magnet created payload'
        daemon.call('session.save')
        daemon.stop()
        daemon.start()
        assert daemon.call('d.state', hash_) == expected


def metadata_restart(daemon):
    compile_peer(daemon)
    for index, declared in enumerate((1, 0)):
        name = 'metadata-restart-' + str(index)
        info = {'length': 8, 'name': name, 'piece length': 16384,
                'pieces': hashlib.sha1(b'xxxxxxxx').digest()}
        hash_ = hashlib.sha1(encode(info)).hexdigest().upper()
        (daemon.directory / (name + '.info')).write_bytes(encode(info))
        daemon.call('load.start' if declared else 'load.normal', '', 'magnet:?xt=urn:btih:' + hash_)
        daemon.wait(lambda: daemon.call('d.is_meta', hash_) == 1, 'pending placeholder')
        if not declared:
            daemon.call('d.start', hash_)
        daemon.call('session.save')
        daemon.stop()
        daemon.start()
        assert daemon.call('d.is_meta', hash_) == 1
        assert daemon.call('d.state', hash_) == 1
        port = 45570 + index
        docker('exec', '-d', daemon.name, '/data/metadata-peer', '/data/' + name + '.info',
               str(port), '/data/' + name + '.ready')
        daemon.wait(lambda: (daemon.directory / (name + '.ready')).exists(), 'local restarted metadata peer')
        daemon.call('add_peer', hash_, '127.0.0.1:' + str(port))
        daemon.wait(lambda: daemon.call('d.is_meta', hash_) == 0, 'metadata conversion after restart')
        assert daemon.call('d.state', hash_) == declared, 'declared metadata start lost after restart'
        if not declared:
            assert daemon.call('d.is_active', hash_) == 0
            assert not (daemon.directory / 'dl' / name).exists()


def stale_counter(daemon):
    hash_ = daemon.fixture('counter-baseline')
    metainfo = (daemon.directory / 'counter-baseline.torrent').read_bytes()
    daemon.call('d.stop', hash_)
    daemon.call('d.start', hash_)
    daemon.call('d.erase', hash_)
    journal = daemon.session / '.rutorrent-stop-intents' / hash_
    assert not journal.exists()
    journal.write_bytes(b'0000000000000008\n')
    journal.chmod(0o600)
    fresh = daemon.directory / 'counter-fresh.torrent'
    fresh.write_bytes(metainfo)
    daemon.call('load.start', '', '/data/counter-fresh.torrent')
    daemon.wait(lambda: daemon.call('d.hash', hash_) == hash_, 'fresh same-hash load')
    assert daemon.call('d.state', hash_) == 1
    daemon.call('session.save')
    daemon.stop()
    daemon.start()
    assert daemon.call('d.state', hash_) == 1


def unsafe_async_path(daemon):
    daemon.stop()
    ancestor = daemon.directory / 'ancestor'
    ancestor.mkdir(mode=0o755)
    daemon.session.rename(ancestor / '.session')
    daemon.session = ancestor / '.session'
    rc = daemon.directory / 'test.rc'
    rc.write_text(rc.read_text().replace('session.path.set=/data/.session',
                                         'session.path.set=/data/ancestor/.session'))
    daemon.start()
    unrelated = daemon.fixture('unrelated')
    ancestor.chmod(0o777)
    # The actor is a different Unix UID; same-UID processes remain trusted.
    docker('exec', '--user', '1001:1001', daemon.name, 'sh', '-c',
           'mv /data/ancestor/.session /data/ancestor/.original-session && '
           'mkdir -m 0700 /data/ancestor/.session')
    info = {'length': 8, 'name': 'refused', 'piece length': 16384,
            'pieces': hashlib.sha1(b'xxxxxxxx').digest()}
    hash_ = hashlib.sha1(encode(info)).hexdigest().upper()
    (daemon.directory / 'refused.torrent').write_bytes(encode({'info': info}))
    daemon.call('load.start', '', '/data/refused.torrent')
    daemon.wait(lambda: 'RR load initialization refused: ' + hash_ in
                (daemon.directory / 'native.log').read_text() or
                'RR migration reservation load refused: ' + hash_ in
                (daemon.directory / 'native.log').read_text(), 'visible asynchronous refusal')
    assert daemon.call('system.pid') > 0
    assert daemon.call('d.hash', unrelated) == unrelated
    assert hash_ not in [row[0] for row in daemon.call('d.multicall2', '', 'main', 'd.hash=')]
    assert '/data/ancestor' in (daemon.directory / 'native.log').read_text()


def erase_guard_preflight(daemon):
    hash_ = daemon.fixture('erase-guard')
    unrelated = daemon.fixture('erase-unrelated')
    daemon.call('method.set_key', '', 'event.download.erased', 'review-path-change',
                'execute.nothrow=chmod,0755,/data/.session/.retrackers-migration')
    try:
        daemon.call('d.erase', hash_)
        assert hash_ not in [row[0] for row in daemon.call('d.multicall2', '', 'main', 'd.hash=')]
        try:
            daemon.call('d.hash', hash_)
            raise AssertionError('erased torrent still responds outside main view')
        except xmlrpc.client.Fault as error:
            assert 'info-hash not found' in error.faultString
        assert daemon.call('d.hash', unrelated) == unrelated
    finally:
        (daemon.session / '.retrackers-migration').chmod(0o700)
        daemon.call('method.set_key', '', 'event.download.erased', 'review-path-change')


def owner_cut(daemon):
    hash_ = daemon.fixture('owner-cut')
    daemon.call('d.custom.set', hash_, 'cut-witness', 'kept')
    tx, incarnation = daemon.begin(hash_)
    try:
        daemon.call('d.retrackers.finish_original', hash_, tx, incarnation)
    except (OSError, RuntimeError):
        pass
    daemon.wait(lambda: docker('inspect', '-f', '{{.State.Running}}', daemon.name).stdout.strip() == 'false',
                'destructive test cut')
    assert docker('inspect', '-f', '{{.State.ExitCode}}', daemon.name).stdout.strip() == '137'
    docker('rm', daemon.name)
    daemon.cut = ''
    daemon.start()
    assert daemon.call('d.state', hash_) == 1
    assert daemon.call('d.custom', hash_, 'cut-witness') == 'kept'
    assert daemon.call('d.custom', hash_, 'retrackers-recovery') == ''
    assert daemon.call('system.retrackers.status', '', hash_) == 'finalized'
    pointer = daemon.session / '.retrackers-migration' / (hash_ + '.owner.current')
    assert pointer.read_text().startswith('ordinary:')
    daemon.call('d.erase', hash_)
    assert not daemon.call('d.multicall2', '', 'main', 'd.hash=')


def owner_cut_retry(daemon):
    hash_ = daemon.fixture('owner-cut-retry')
    tx, incarnation = daemon.begin(hash_)
    try:
        daemon.call('d.retrackers.finish_original', hash_, tx, incarnation)
        raise AssertionError('test-cut build did not throw')
    except xmlrpc.client.Fault as error:
        assert 'RR test owner ordinary publication failure.' in error.faultString
    pointer = daemon.session / '.retrackers-migration' / (hash_ + '.owner.current')
    assert pointer.read_text().startswith('active:')
    assert daemon.call('d.retrackers.finish_original', hash_, tx, incarnation) == 'finalized'
    assert pointer.read_text().startswith('ordinary:')
    assert daemon.call('d.state', hash_) == 1
    assert daemon.call('d.custom', hash_, 'retrackers-recovery') == ''
    daemon.call('d.erase', hash_)
    assert not daemon.call('d.multicall2', '', 'main', 'd.hash=')


CASES = {'stop-cat': stop_cat, 'claim-restart': claim_restart,
         'claim-finish': claim_finish, 'owner-finalize': owner_finalize,
         'metadata-start': metadata_start, 'metadata-restart': metadata_restart,
         'stale-counter': stale_counter,
         'unsafe-async-path': unsafe_async_path, 'erase-guard-preflight': erase_guard_preflight}
CASES.update({'owner-cut-before': owner_cut, 'owner-cut-after': owner_cut,
              'owner-cut-retry': owner_cut_retry})


if __name__ == '__main__':
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    image, tree, scratch = sys.argv[1:4]
    selected = sys.argv[4:] or [case for case in CASES if not case.startswith('owner-cut-')]
    failures = []
    for case in selected:
        daemon = Daemon(image, tree, scratch, case)
        try:
            daemon.start()
            CASES[case](daemon)
            print('PASS - ' + case, flush=True)
        except Exception as error:
            failures.append(case)
            print('FAIL - ' + case + ': ' + str(error), flush=True)
            (daemon.directory / 'container.log').write_text(docker('logs', daemon.name, check=False).stdout)
        finally:
            docker('rm', '-f', daemon.name, check=False)
    raise SystemExit(bool(failures))

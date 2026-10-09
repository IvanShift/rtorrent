#!/usr/bin/env python3
"""Run native review regressions against exact locally built daemon bytes.

Usage: native_regressions.py [OPTION ...] BUILDER_IMAGE BUILD_TREE SCRATCH [CASE ...]
The image must supply the matching libtorrent ABI and a C++ compiler. Each case
runs in a disposable network-isolated container; no public trackers are used.

Without CASE arguments every case that needs no test cut runs. Options:
  --cuts          also run the cases that need a build configured with
                  --enable-rr-migration-test-cuts (the CUTS table below)
  --ninep         emulate WSL drvfs over 9p: a seccomp profile makes every
                  flagged renameat2 fail with EINVAL
  --prefix=NAME   container name prefix (default rr-remediation-)
  --label=K=V     extra container label; may repeat
"""
import datetime
import hashlib
import json
import os
import re
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


SETTINGS = {'prefix': 'rr-remediation-', 'labels': [], 'seccomp': ''}
# The one list of cases that need a test-cut build, with the RR_MIG_TEST_CUT
# each sets. Only --cuts (or naming the case) runs them.
CUTS = {'move-sync-failure': 'move-directory-sync-failed',
        'move-publish-before': 'before-move-record-publication',
        'move-publish-after': 'after-move-record-publication',
        'move-finish-crash': 'after-move-finish',
        'move-claim-crash': 'after-stop-claim-publication',
        'move-supersede-crash': 'after-stop-claim-publication',
        'samehash-prepared-prior': 'after-samehash-prepared',
        'samehash-prepared-foreign': 'after-samehash-prepared',
        'owner-cut-before': 'before-owner-ordinary',
        'owner-cut-after': 'after-owner-ordinary',
        'owner-cut-retry': 'before-owner-ordinary-throw',
        'shutdown-early-save': 'after-shutdown-session-save',
        'shutdown-hook-save': 'after-shutdown-session-save',
        'captured-remove': 'after-capture-intent-throw',
        'defer-crash': 'after-original-defer',
        'owner-tombstone-cut': 'after-owner-tombstone-receipts',
        'defer-late-failure-cut': 'after-finalize-prune-throw',
        'capture-crash-intent': 'after-capture-intent',
        'capture-crash-quiesced': 'after-capture-quiesced',
        'session-save-overlap-held': 'slow-session-save',
        **{'rename-' + error: 'rename-noreplace-' + error
           for error in ('einval', 'enosys', 'eopnotsupp')}}
# Flagged renameat2 fails with EINVAL, as WSL drvfs over 9p answers it.
NINEP_SECCOMP = {'defaultAction': 'SCMP_ACT_ALLOW', 'syscalls': [{
    'names': ['renameat2'], 'action': 'SCMP_ACT_ERRNO', 'errnoRet': 22,
    'args': [{'index': 4, 'value': 0, 'op': 'SCMP_CMP_NE'}]}]}


def container_options():
    return [option for label in SETTINGS['labels'] for option in ('--label', label)]


class Daemon:
    def __init__(self, image, tree, scratch, case):
        self.image, self.tree = image, pathlib.Path(tree).resolve()
        self.name = SETTINGS['prefix'] + uuid.uuid4().hex[:12]
        self.cut = CUTS.get(case, '')
        self.environment = {}
        self.options = []  # Extra docker run options, such as a tmpfs session.
        self.directory = pathlib.Path(scratch).resolve() / (case + '-' + uuid.uuid4().hex[:8])
        self.directory.mkdir(mode=0o755, parents=True)
        self.session = self.directory / '.session'
        self.session.mkdir(mode=0o2700)
        self.session.chmod(0o2700)  # mkdir(2) may discard setgid from its mode argument.
        assert self.session.stat().st_mode & 0o7777 == 0o2700
        (self.directory / 'dl').mkdir()
        self.socket = self.directory / 'rtorrent.sock'
        (self.directory / 'test.rc').write_text('\n'.join([
            'system.daemon.set=true', 'session.path.set=/data/.session',
            'directory.default.set=/data/dl', 'network.scgi.open_local=/data/rtorrent.sock',
            'network.listen.port.range.set=45561-45561', 'network.block.ipv6.set=no',
            'dht.mode.set=disable', 'protocol.pex.set=no', 'trackers.use_udp.set=no',
            'log.open_file.flush="native",/data/native.log', 'log.add_output="error","native"',
            'log.add_output="torrent_events","native"', 'log.add_output="warn","native"', '']))

    def start(self):
        self.run()
        self.wait(lambda: self.call('system.pid') > 0, 'SCGI readiness')

    def run(self):
        self.socket.unlink(missing_ok=True)
        (self.session / 'rtorrent.lock').unlink(missing_ok=True)
        seccomp = ['--security-opt', 'seccomp=' + SETTINGS['seccomp']] if SETTINGS['seccomp'] else []
        docker('run', '-d', '--pull=never', '--name', self.name, '--network', 'none',
               *container_options(), *seccomp,
               '--stop-timeout', '60', '--user', f'{os.getuid()}:{os.getgid()}', '-e', 'HOME=/tmp',
               '-e', 'RR_MIG_TEST_CUT=' + self.cut,
               *[option for key, value in self.environment.items() for option in ('-e', key + '=' + value)],
               *self.options,
               '-v', str(self.tree) + ':/w:ro', '-v', str(self.directory) + ':/data',
               '--entrypoint', '/w/src/rtorrent', self.image, '-n', '-o', 'import=/data/test.rc')

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
    raw = (daemon.directory / 'owner-finalize.torrent').read_bytes()
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
    # A batch shortly after the erase retires the owner's tombstone and
    # finalized receipt once the ordinary triple is durably gone, so erased
    # hashes do not accumulate.
    journal = daemon.session / '.retrackers-migration'
    try:
        daemon.wait(lambda: not any(x.name.startswith(hash_) for x in journal.iterdir()),
                    'erased owner retirement batch', timeout=10)
    except AssertionError:
        left = sorted(x.name for x in journal.iterdir() if x.name.startswith(hash_))
        raise AssertionError('erased owner state kept: ' + repr(left))
    assert not (daemon.session / '.rutorrent-stop-intents' / hash_).exists(), 'stop intent kept'
    daemon.stop()
    daemon.start()
    assert not daemon.call('d.multicall2', '', 'main', 'd.hash='), 'retired owner resurrected'
    # The same metainfo can return and finish a fresh claim.
    daemon.call('load.raw_start', '', xmlrpc.client.Binary(raw))
    daemon.wait(lambda: daemon.call('d.hash', hash_) == hash_, 're-added owner')
    tx, incarnation = daemon.begin(hash_)
    assert daemon.call('d.retrackers.finish_original', hash_, tx, incarnation) == 'finalized'
    assert pointer.read_text().startswith('ordinary:'), pointer.read_text()


def startup_hold_visible(daemon):
    # A daemon without a UI shows only its log files: a startup hold names its
    # hash and reason in the error log, not just in the UI log buffer.
    hash_ = daemon.fixture('startup-hold')
    tx, incarnation = daemon.begin(hash_)
    assert daemon.call('d.retrackers.finish_original', hash_, tx, incarnation) == 'finalized'
    daemon.stop()
    receipt = daemon.session / '.retrackers-migration' / (hash_ + '.finalized.receipt')
    receipt.write_text('finalized:malformed\n')
    daemon.start()
    log = (daemon.directory / 'native.log').read_text()
    assert 'RR finalized owner held: ' + hash_ + ': ' in log, log[-2000:]


def owner_tombstone_cut(daemon):
    # A crash after the finalized receipt went, before the tombstone did,
    # leaves a tombstone alone; the next startup completes the retirement.
    hash_ = daemon.fixture('owner-tombstone')
    tx, incarnation = daemon.begin(hash_)
    assert daemon.call('d.retrackers.finish_original', hash_, tx, incarnation) == 'finalized'
    journal = daemon.session / '.retrackers-migration'
    pointer = journal / (hash_ + '.owner.current')
    receipt = journal / (hash_ + '.finalized.receipt')
    try:
        daemon.call('d.erase', hash_)
    except (OSError, RuntimeError):
        pass
    daemon.wait(lambda: docker('inspect', '-f', '{{.State.Running}}', daemon.name).stdout.strip() == 'false',
                'tombstone retirement cut')
    assert docker('inspect', '-f', '{{.State.ExitCode}}', daemon.name).stdout.strip() == '137'
    docker('rm', daemon.name)
    assert not receipt.exists() and pointer.read_text().startswith('retired:')
    daemon.cut = ''
    daemon.start()
    assert not daemon.call('d.multicall2', '', 'main', 'd.hash='), 'retired owner resurrected'
    left = sorted(x.name for x in journal.iterdir() if x.name.startswith(hash_))
    assert not left, 'interrupted tombstone retirement left: ' + repr(left)
    assert daemon.call('system.retrackers.status', '', hash_) == 'none'


def owner_tombstone_startup(daemon):
    # Tombstones that older daemons left for erased owners, and pointers of
    # ordinary owners without a triple, are retired at startup; a tombstone
    # whose hash another journal entry still names stays.
    journal = daemon.session / '.retrackers-migration'
    daemon.stop()

    def tombstone(hash_, extra=None):
        incarnation, tx = uuid.uuid4().hex, uuid.uuid4().hex
        files = {hash_ + '.owner.current': f'retired:{hash_}:{incarnation}\n',
                 hash_ + '.owner.' + incarnation + '.late.receipt': 'stale backup\n'}
        if extra:
            files[hash_ + extra[0]] = extra[1]
        else:
            files[hash_ + '.finalized.receipt'] = f'finalized:{hash_}:{tx}:{incarnation}\n'
        for name, text in files.items():
            (journal / name).write_text(text)
            (journal / name).chmod(0o600)
        return sorted(files)

    erased = [hashlib.sha1(str(index).encode()).hexdigest().upper() for index in range(3)]
    for hash_ in erased:
        tombstone(hash_)
    # An ordinary owner whose triple is gone is not loaded and owes nothing.
    orphan = hashlib.sha1(b'orphan').hexdigest().upper()
    for name in tombstone(orphan):
        if name.endswith('.owner.current'):
            text = (journal / name).read_text().replace('retired:', 'ordinary:', 1)
            (journal / name).write_text(text)
    erased.append(orphan)
    # A successor reservation still names this hash: its tombstone stays.
    kept_hash = hashlib.sha1(b'kept').hexdigest().upper()
    kept = tombstone(kept_hash, ('.target', erased[0] + '\n'))
    daemon.start()
    left = sorted(x.name for x in journal.iterdir() if x.name[:40] in erased)
    assert not left, 'erased owner tombstones survived startup: ' + repr(left)
    assert sorted(x.name for x in journal.iterdir() if x.name.startswith(kept_hash)) == kept


def compile_fixture(daemon, source, output, libraries=''):
    path = pathlib.Path(__file__).with_name(source)
    docker('run', '--rm', '--pull=never', '--network', 'none',
           '--name', SETTINGS['prefix'] + 'compile-' + uuid.uuid4().hex[:8], *container_options(),
           '--user', f'{os.getuid()}:{os.getgid()}', '-v', str(path) + ':/fixture.cc:ro',
           '-v', str(daemon.directory) + ':/data', '--entrypoint', '/bin/sh', daemon.image,
           '-c', 'g++ -std=c++20 -Wall -Wextra -Werror /fixture.cc ' + libraries + ' -o /data/' + output)


def compile_peer(daemon):
    compile_fixture(daemon, 'metadata-peer.cc', 'metadata-peer', '-lcrypto')


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
    untrusted = 1001 if os.getuid() != 1001 else 1002
    docker('exec', '--user', f'{untrusted}:{untrusted}', daemon.name, 'sh', '-c',
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
    docker('exec', '--user', f'{untrusted}:{untrusted}', daemon.name,
           'rmdir', '/data/ancestor/.session')
    (ancestor / '.original-session').rename(daemon.session)
    ancestor.chmod(0o755)


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


def enqueue(daemon, hash_):
    local = daemon.call('d.local_id', hash_)
    marker = daemon.call('d.retrackers.enqueue_original', hash_, local, '0' * 64,
                         str(daemon.call('d.state', hash_)))
    queue = daemon.call('d.custom', hash_, 'retrackers-queued')
    return local, marker, marker.rsplit(':', 1)[1], daemon.call('d.incarnation', hash_), queue[68:]


def expect_fault(operation, detail):
    try:
        operation()
    except xmlrpc.client.Fault as error:
        assert detail in error.faultString, error.faultString
    else:
        raise AssertionError('Expected refusal: ' + detail)


def rename_fallback(daemon):
    hash_ = daemon.fixture('rename-claim')
    token = daemon.call('d.stop_close_claim', hash_, 'b' * 32)
    assert token and daemon.call('d.state', hash_) == 0
    receipt = daemon.session / '.rutorrent-stop-intents' / (hash_ + '.claim')
    assert receipt.stat().st_nlink == 1
    unrelated = daemon.fixture('rename-unrelated')
    hash_ = daemon.fixture('rename-capsule')
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    daemon.call('d.retrackers.capture_erase', hash_, local, marker, hash_, tx, '0' * 64)
    assert hash_ not in [r[0] for r in daemon.call('d.multicall2', '', 'main', 'd.hash=')]
    journal = daemon.session / '.retrackers-migration'
    for suffix in ('.rollback.torrent', '.rollback.torrent.rtorrent',
                   '.rollback.torrent.libtorrent_resume', '.benc', '.backup.benc', '.target'):
        assert (journal / (hash_ + suffix)).stat().st_nlink == 1
    assert daemon.call('d.hash', unrelated) == unrelated
    assert daemon.session.stat().st_mode & 0o7777 == 0o2700
    # Seven publications above; the classified fallback report appears once.
    reports = [line for line in (daemon.directory / 'native.log').read_text().splitlines()
               if 'checked rename fallback' in line]
    fallback = bool(daemon.cut) or bool(SETTINGS['seccomp'])
    assert len(reports) == (1 if fallback else 0), reports
    if fallback:
        assert reports[0].endswith('reported once per process'), reports


def uncaptured_remove(daemon):
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    hash_ = daemon.fixture('remove-uncaptured', start=False)
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    assert 'wp:' + local in daemon.call('method.list_keys', '', 'rr.receipts.v1')
    daemon.call('d.erase', hash_)
    assert hash_ not in [r[0] for r in daemon.call('d.multicall2', '', 'main', 'd.hash=')]
    assert 'wp:' + local not in daemon.call('method.list_keys', '', 'rr.receipts.v1')
    assert not (daemon.session / (hash_ + '.torrent')).exists()
    daemon.stop()
    daemon.start()
    assert hash_ not in [r[0] for r in daemon.call('d.multicall2', '', 'main', 'd.hash=')]


def retry_cancel(daemon):
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    hash_ = daemon.fixture('retry-cancel', start=False)
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    first = f'r1:{queue}:1:{int(time.time()) + 60}:capture-refused'
    cancel = lambda expected, new: daemon.call('d.retrackers.defer_original', hash_, tx, incarnation,
                                              queue, expected, new)
    expect_fault(lambda: cancel('stale', first), 'retry state')
    expect_fault(lambda: cancel('', first.replace('capture-refused', 'untrusted-text')), 'retry state')
    expect_fault(lambda: cancel('', first.replace(':1:', ':2:')), 'advance once')
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == ''
    assert daemon.call('d.custom', hash_, 'retrackers-recovery') == marker
    assert cancel('', first) == 'cancelled'
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == first
    assert daemon.call('d.custom', hash_, 'retrackers-recovery') == ''
    assert daemon.call('d.custom', hash_, 'retrackers-queued').endswith(queue)
    assert daemon.call('d.custom', hash_, 'retrackers-queue-finished') == ''
    assert 'wp:' + local not in daemon.call('method.list_keys', '', 'rr.receipts.v1')
    daemon.stop(kill=True)
    daemon.start()
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == first
    assert daemon.call('d.custom', hash_, 'retrackers-queued').endswith(queue)
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    previous = first
    for count in range(2, 7):
        local, marker, tx, incarnation, same_queue = enqueue(daemon, hash_)
        assert same_queue == queue
        assert 'wp:' + local in daemon.call('method.list_keys', '', 'rr.receipts.v1')
        due = 0 if count == 6 else int(time.time()) + 60
        retry = f'r1:{queue}:{count}:{due}:source-not-ready'
        assert cancel(previous, retry) == 'cancelled'
        previous = retry
    assert 'retries exhausted' in daemon.call('d.message', hash_)
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == previous
    # The daemon's own volatile status must follow durable retry/Q changes too.
    daemon.call('d.custom.set', hash_, 'retrackers-retry', '')
    assert daemon.call('d.message', hash_) == '', 'cleared retry retained stale owned exhaustion status'
    daemon.call('d.custom.set', hash_, 'retrackers-retry', previous)
    old_queue = daemon.call('d.custom', hash_, 'retrackers-queued')
    daemon.call('d.custom.set', hash_, 'retrackers-queued', old_queue[:68] + 'f' * 32)
    assert daemon.call('d.message', hash_) == '', 'volatile exhaustion escaped its exact queue generation'
    daemon.call('d.custom.set', hash_, 'retrackers-queued', old_queue)
    daemon.call('d.message.set', hash_, '')
    assert 'retries exhausted' in daemon.call('d.message', hash_), 'terminal status lost when tracker clears message'
    daemon.call('d.message.set', hash_, 'independent operator message')
    assert daemon.call('d.message', hash_) == 'independent operator message'
    daemon.call('d.message.set', hash_, '')
    daemon.stop(kill=True)
    daemon.start()
    assert 'retries exhausted' in daemon.call('d.message', hash_), 'durable terminal status lost after restart'
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == previous
    old_queue = daemon.call('d.custom', hash_, 'retrackers-queued')
    daemon.call('d.custom.set', hash_, 'retrackers-queued', old_queue[:68] + 'f' * 32)
    assert daemon.call('d.message', hash_) == '', 'terminal status escaped its exact queue generation'
    daemon.call('d.custom.set', hash_, 'retrackers-queued', old_queue)
    daemon.call('d.erase', hash_)


def retry_wait(daemon):
    # Temporary unreadiness is a wait: a wait reason may keep the attempt count
    # with a future due. Counted reasons, the +1 step of applications without
    # waits, and the terminal sixth attempt keep their rules.
    assert daemon.call('system.retrackers.retry_waits', '') == 1
    assert 'system.retrackers.retry_waits' in daemon.call('system.listMethods')
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    hash_ = daemon.fixture('retry-wait', start=False)
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    later = lambda seconds=60: int(time.time()) + seconds

    def defer(expected, new):
        return daemon.call('d.retrackers.defer_original', hash_, tx, incarnation, queue, expected, new)

    previous = f'r1:{queue}:1:{later()}:transport-unavailable'
    assert defer('', previous) == 'cancelled'
    for reason in ('source-not-ready', 'transport-unavailable'):
        local, marker, tx, incarnation, _ = enqueue(daemon, hash_)
        wait = f'r1:{queue}:1:{later(90)}:{reason}'
        assert defer(previous, wait) == 'cancelled', 'wait refused: ' + reason
        assert daemon.call('d.custom', hash_, 'retrackers-retry') == wait
        previous = wait
    local, marker, tx, incarnation, _ = enqueue(daemon, hash_)
    for reason in ('capture-refused', 'preflight-refused'):
        expect_fault(lambda: defer(previous, f'r1:{queue}:1:{later(120)}:{reason}'), 'advance once or wait')
    expect_fault(lambda: defer(previous, f'r1:{queue}:1:0:source-not-ready'), 'retry state')
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == previous
    for count in range(2, 7):
        if count > 2:
            local, marker, tx, incarnation, _ = enqueue(daemon, hash_)
        new = f'r1:{queue}:{count}:{0 if count == 6 else later()}:capture-refused'
        assert defer(previous, new) == 'cancelled', 'counted step refused at ' + str(count)
        previous = new
    assert 'retries exhausted' in daemon.call('d.message', hash_)
    local, marker, tx, incarnation, _ = enqueue(daemon, hash_)
    expect_fault(lambda: defer(previous, f'r1:{queue}:6:{later()}:source-not-ready'), 'retry state')
    expect_fault(lambda: defer(previous, f'r1:{queue}:5:{later()}:source-not-ready'), 'advance once or wait')
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == previous


def retry_new_queue(daemon):
    hash_ = daemon.fixture('retry-new-queue', start=False)
    stale = 'r1:' + 'f' * 32 + ':6:0:source-not-ready'
    daemon.call('d.custom.set', hash_, 'retrackers-retry', stale)
    daemon.call('d.message.set', hash_,
                'retrackers: retries exhausted; inspect d.custom=retrackers-retry; queued work retained')
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == ''
    assert daemon.call('d.message', hash_) == ''
    assert daemon.call('d.retrackers.cancel_original', hash_, tx, incarnation, queue) == 'cancelled'
    daemon.call('d.erase', hash_)
    hash_ = daemon.fixture('retry-new-queue-message', start=False)
    daemon.call('d.custom.set', hash_, 'retrackers-retry', stale)
    daemon.call('d.message.set', hash_, 'independent operator message')
    enqueue(daemon, hash_)
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == ''
    assert daemon.call('d.message', hash_) == 'independent operator message'


def captured_remove(daemon):
    hash_ = daemon.fixture('captured-remove')
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    expect_fault(lambda: daemon.call('d.retrackers.capture_erase', hash_, local, marker, hash_,
                                    tx, '0' * 64), 'captured intent held')
    retry = f'r1:{queue}:1:{int(time.time()) + 60}:capture-refused'
    # cancel_original keeps exactly three arguments; a retry CAS is defer_original.
    expect_fault(lambda: daemon.call('d.retrackers.cancel_original', hash_, tx, incarnation,
                                    queue, '', retry), 'needs exact transaction')
    expect_fault(lambda: daemon.call('d.retrackers.defer_original', hash_, tx, incarnation,
                                    queue), 'defer needs')
    expect_fault(lambda: daemon.call('d.retrackers.defer_original', hash_, tx, incarnation,
                                    queue, '', retry), 'RR original defer is blocked by capture')
    expect_fault(lambda: daemon.call('d.retrackers.cancel_original', hash_, tx, incarnation,
                                    queue), 'RR original cancel is blocked by capture')
    expect_fault(lambda: daemon.call('d.erase', hash_),
                 'RR original claim blocks direct erase: RR original cancel is blocked by capture')
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == ''
    assert daemon.call('d.hash', hash_) == hash_
    assert daemon.call('d.custom', hash_, 'retrackers-recovery') == marker
    daemon.call('session.save')


def partial_capture_remove(daemon):
    hash_ = daemon.fixture('partial-capture-remove')
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    journal = daemon.session / '.retrackers-migration'
    partial = journal / (hash_ + '.rollback.torrent')
    partial.write_bytes(b'partial captured snapshot')
    partial.chmod(0o600)
    # The dispatcher status agrees with every guard: partial publication is held.
    status = daemon.call('system.retrackers.original_claim', '', hash_)
    assert status == 'held', 'partial publication reported as ' + status[:9]
    before = sorted(x.name for x in journal.iterdir())
    expect_fault(lambda: daemon.call('d.retrackers.capture_erase', hash_, local, marker, hash_,
                                    tx, '0' * 64), 'blocked by an existing capture')
    assert sorted(x.name for x in journal.iterdir()) == before, 'refused capture changed the journal'
    retry = f'r1:{queue}:1:{int(time.time()) + 60}:capture-refused'
    expect_fault(lambda: daemon.call('d.retrackers.defer_original', hash_, tx, incarnation,
                                    queue, '', retry), 'blocked by capture')
    expect_fault(lambda: daemon.call('d.retrackers.finish_original', hash_, tx, incarnation),
                 'blocked by capture')
    expect_fault(lambda: daemon.call('d.erase', hash_),
                 'RR original claim blocks direct erase: RR original cancel is blocked by capture')
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == ''
    assert partial.read_bytes() == b'partial captured snapshot'
    assert daemon.call('d.hash', hash_) == hash_
    assert daemon.call('d.custom', hash_, 'retrackers-recovery') == marker
    # A capsule part without any claim refuses a new queued claim too.
    other = daemon.fixture('partial-capture-enqueue')
    orphan = journal / (other + '.backup.benc')
    orphan.write_bytes(b'orphan capsule part')
    orphan.chmod(0o600)
    expect_fault(lambda: enqueue(daemon, other), 'prior authority must be retired')
    assert daemon.call('d.custom', other, 'retrackers-queued') == ''
    assert daemon.call('system.retrackers.original_claim', '', other) == 'none'
    # Unknown capture stays held; only migration recovery can resolve it.


def capture_existing_capsule(daemon):
    # Same-hash capsule names that already exist are refused before any side
    # effect: an existing file and a dangling symlink stay byte-for-byte.
    journal = daemon.session / '.retrackers-migration'
    for kind, suffix in (('file', '.backup.benc'), ('symlink', '.target')):
        hash_ = daemon.fixture('capture-existing-' + kind)
        local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
        victim = journal / (hash_ + suffix)
        if kind == 'file':
            victim.write_bytes(b'retain-me')
            victim.chmod(0o600)
        else:
            os.symlink('missing-target', victim)
        before = sorted(x.name for x in journal.iterdir())
        expect_fault(lambda: daemon.call('d.retrackers.capture_erase', hash_, local, marker, hash_,
                                        tx, '0' * 64), 'blocked by an existing capture')
        if kind == 'file':
            assert victim.read_bytes() == b'retain-me', 'existing capsule file overwritten'
        else:
            assert victim.is_symlink() and os.readlink(victim) == 'missing-target'
        assert sorted(x.name for x in journal.iterdir()) == before, 'refused capture changed the journal'
        assert daemon.call('d.hash', hash_) == hash_
        assert daemon.call('system.retrackers.original_claim', '', hash_) == 'held'


def defer_crash(daemon):
    hash_ = daemon.fixture('defer-crash', start=False)
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    retry = f'r1:{queue}:1:{int(time.time()) + 60}:capture-refused'
    try:
        daemon.call('d.retrackers.defer_original', hash_, tx, incarnation, queue, '', retry)
        raise AssertionError('defer crash cut was not reached')
    except (OSError, RuntimeError):
        pass
    daemon.wait(lambda: docker('inspect', '-f', '{{.State.Running}}', daemon.name).stdout.strip() == 'false',
                'durable deferred cancellation crash')
    assert docker('inspect', '-f', '{{.State.ExitCode}}', daemon.name).stdout.strip() == '137'
    docker('rm', daemon.name)
    daemon.cut = ''
    daemon.start()
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == retry
    assert daemon.call('d.custom', hash_, 'retrackers-queued').endswith(queue)
    assert daemon.call('d.custom', hash_, 'retrackers-recovery') == ''
    assert daemon.call('d.custom', hash_, 'retrackers-recovery-ack') == ''
    assert daemon.call('d.custom', hash_, 'retrackers-queue-finished') == ''
    daemon.call('d.erase', hash_)


def shutdown_early_save(daemon, hook=False):
    witness = {}
    for index in range(29):
        hash_ = daemon.fixture('shutdown-save-' + str(index), start=False)
        witness[hash_] = 'latest-label-' + str(index)
    # Initial full saves must finish before the latest edits, which get no
    # explicit save command and must survive solely through shutdown handling.
    daemon.call('session.save')
    daemon.wait(lambda: all((daemon.session / (h + '.torrent.rtorrent')).exists() for h in witness),
                'initial save triples')
    if hook:
        # The docker-rutorrent rc hook, kept for 0.9.8 and unpatched daemons.
        daemon.call('method.set_key', '', 'event.system.shutdown', 'docker_rutorrent_early_save', 'session.save=')
    for hash_, value in witness.items():
        daemon.call('d.custom1.set', hash_, value)
    docker('kill', '--signal', 'SIGINT', daemon.name)
    daemon.wait(lambda: docker('inspect', '-f', '{{.State.Running}}', daemon.name).stdout.strip() == 'false',
                'early save destructive cut')
    assert docker('inspect', '-f', '{{.State.ExitCode}}', daemon.name).stdout.strip() == '137'
    docker('rm', daemon.name)
    daemon.cut = ''
    daemon.start()
    for hash_, value in witness.items():
        assert daemon.call('d.custom1', hash_) == value, hash_
    assert daemon.session.stat().st_mode & 0o7777 == 0o2700
    print('  latest shutdown labels restored: 29/29', flush=True)


def dispatcher_gate(daemon):
    # The 1 s dispatcher schedule launches dispatch.php only when a queue of
    # its user has work (rr_dispatch_need in src/command_download.cc).
    compile_fixture(daemon, 'fake-php.cc', 'fake-php')
    scripts = daemon.directory / 'app' / 'plugins' / 'retrackers'
    scripts.mkdir(parents=True)
    for path in (daemon.directory, daemon.directory / 'app', scripts.parent, scripts):
        path.chmod(0o755)
    for script in ('dispatch.php', 'update.php'):
        (scripts / script).write_text('<?php\n')
        (scripts / script).chmod(0o644)
    (daemon.directory / 'fake-php').chmod(0o755)
    daemon.stop()
    daemon.environment = {'RUTORRENT_NATIVE_PHP': '/data/fake-php', 'RUTORRENT_NATIVE_APP_ROOT': '/data/app'}
    daemon.start()
    user = 'gateuser'
    digest = hashlib.sha256(user.encode()).hexdigest()
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    daemon.call('method.set_key', '', 'rr.receipts.v1', 'pv:' + 'a' * 32, '1')
    daemon.call('method.set_key', '', 'rr.receipts.v1', 'pf:' + digest + ':' + 'b' * 64, '1')
    actors = daemon.directory / 'actor.log'
    passes = lambda: [line for line in (actors.read_text().splitlines() if actors.exists() else [])
                      if '/dispatch.php ' in line]
    dispatch = lambda: daemon.call('system.retrackers.dispatcher_start', '', user)

    def settle():
        # The previous pass exits at once; the next launch reaps it first.
        time.sleep(.5)

    # Idle: the PHP-armed 1 s schedule spawns nothing.
    key = 'rr.queue.' + digest
    daemon.call('schedule.if_absent', '', key, '1', '1', 'system.retrackers.dispatcher_start=' + user)
    time.sleep(5)
    assert passes() == [], 'idle ticks spawned dispatch.php: ' + repr(passes())
    # A new queued job is dispatched within one tick.
    hash_ = daemon.fixture('gate-job', start=False)
    local = daemon.call('d.local_id', hash_)
    marker = daemon.call('d.retrackers.enqueue_original', hash_, local, digest, '0')
    tx, incarnation = marker.rsplit(':', 1)[1], daemon.call('d.incarnation', hash_)
    queue = daemon.call('d.custom', hash_, 'retrackers-queued')[68:]
    daemon.wait(lambda: passes(), 'dispatch within one tick', timeout=3)
    daemon.call('schedule.remove', '', key)
    time.sleep(1.5)
    # A live worker owns its original claim.
    assert daemon.call('system.retrackers.worker_start', '', hash_, tx, incarnation, user) == 'spawned'
    settle()
    assert dispatch() == 'idle', 'pass launched for a job with a live worker'
    # A wait with a future due needs no pass until the due time.
    due = int(time.time()) + 4
    assert daemon.call('d.retrackers.defer_original', hash_, tx, incarnation, queue, '',
                       f'r1:{queue}:1:{due}:source-not-ready') == 'cancelled'
    assert dispatch() == 'idle', 'pass launched before the wait was due'
    while time.time() <= due:
        time.sleep(.2)
    assert dispatch() == 'spawned', 'due wait was not dispatched'
    settle()
    # A terminal retry needs no pass.
    daemon.call('d.custom.set', hash_, 'retrackers-retry', f'r1:{queue}:6:0:capture-refused')
    assert dispatch() == 'idle', 'pass launched for a terminal retry'
    # A finalized claim needs a pass to dequeue; afterwards nothing is left.
    local = daemon.call('d.local_id', hash_)
    marker = daemon.call('d.retrackers.enqueue_original', hash_, local, digest, '0')
    tx, incarnation = marker.rsplit(':', 1)[1], daemon.call('d.incarnation', hash_)
    assert daemon.call('d.retrackers.finish_original', hash_, tx, incarnation) == 'finalized'
    assert dispatch() == 'spawned', 'finalized claim was not dispatched'
    settle()
    assert daemon.call('d.retrackers.dequeue_original', hash_, tx, incarnation, queue) == 'dequeued'
    assert dispatch() == 'idle'
    # Anomalies need a visible pass, at most one a minute.
    other = daemon.fixture('gate-anomaly', start=False)
    daemon.call('d.custom.set', other, 'retrackers-queued', 'q1:malformed')
    assert dispatch() == 'spawned', 'anomaly was not dispatched'
    settle()
    assert dispatch() == 'idle', 'anomaly dispatched twice within a minute'
    print('  dispatch passes: ' + str(len(passes())), flush=True)


def defer_late_failure(daemon):
    # A storage failure in the final cleanup comes after the durable release
    # and finalization, which prunes the release receipt: the published retry
    # stays, so neither the step nor a terminal exhaustion is lost.
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    journal = daemon.session / '.retrackers-migration'
    kept = []
    for index in range(8):
        hash_ = daemon.fixture('late-failure-' + str(index), start=False)
        local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
        prior = f'r1:{queue}:5:{int(time.time()) + 60}:capture-refused'
        terminal = f'r1:{queue}:6:0:capture-refused'
        daemon.call('d.custom.set', hash_, 'retrackers-retry', prior)
        # A directory under the hash's prefix fails the cleanup's unlink.
        plant = journal / (hash_ + '.unremovable')
        plant.mkdir(mode=0o700)
        expect_fault(lambda: daemon.call('d.retrackers.defer_original', hash_, tx, incarnation, queue,
                                        prior, terminal), 'final cleanup removal failed')
        finalized = journal / (hash_ + '.finalized.receipt')
        assert finalized.exists() and finalized.read_text().split(':')[2] == tx, 'release did not finalize'
        retry = daemon.call('d.custom', hash_, 'retrackers-retry')
        assert retry == terminal, f'try {index}: the finalized step was rolled back to {retry[36:]}'
        assert 'retries exhausted' in daemon.call('d.message', hash_)
        plant.rmdir()
        kept.append((hash_, terminal))
    daemon.call('session.save')
    time.sleep(1)
    daemon.stop()
    daemon.start()
    for hash_, terminal in kept:
        assert daemon.call('d.custom', hash_, 'retrackers-retry') == terminal
        assert 'retries exhausted' in daemon.call('d.message', hash_)


def defer_late_failure_cut(daemon):
    # The same rule with the failure placed deterministically after the
    # release receipt was pruned.
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    hash_ = daemon.fixture('late-failure-cut', start=False)
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    journal = daemon.session / '.retrackers-migration'
    prior = f'r1:{queue}:5:{int(time.time()) + 60}:capture-refused'
    terminal = f'r1:{queue}:6:0:capture-refused'
    daemon.call('d.custom.set', hash_, 'retrackers-retry', prior)
    expect_fault(lambda: daemon.call('d.retrackers.defer_original', hash_, tx, incarnation, queue,
                                    prior, terminal), 'final cleanup failure after prune')
    assert not (journal / (hash_ + '.released.receipt')).exists()
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == terminal, 'finalized step rolled back'
    assert 'retries exhausted' in daemon.call('d.message', hash_)
    assert daemon.call('system.retrackers.original_claim', '', hash_) == 'none'
    daemon.call('session.save')
    time.sleep(1)
    daemon.stop()
    daemon.cut = ''
    daemon.start()
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == terminal
    assert 'retries exhausted' in daemon.call('d.message', hash_)


def erase_storage_refusal(daemon):
    # A storage failure while erase cancels a queued claim refuses the erase
    # with its context and keeps the download, its claim and marker.
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    hash_ = daemon.fixture('erase-storage', start=False)
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    journal = daemon.session / '.retrackers-migration'
    prefix = hash_ + '.claim.' + tx
    receipt = journal / (prefix + '.late.receipt')
    sequence = int(receipt.read_text().split(':')[4]) + 1 if receipt.exists() else 1
    blocker = journal / (prefix + '.late.' + str(sequence) + '.a.torrent')
    blocker.mkdir(mode=0o700)
    expect_fault(lambda: daemon.call('d.erase', hash_),
                 'RR original claim blocks direct erase: failed to rename torrent file')
    assert daemon.call('d.hash', hash_) == hash_
    assert daemon.call('system.retrackers.original_claim', '', hash_).startswith('original:' + tx + ':')
    assert daemon.call('d.custom', hash_, 'retrackers-recovery') == marker
    blocker.rmdir()
    daemon.call('d.erase', hash_)
    assert hash_ not in [r[0] for r in daemon.call('d.multicall2', '', 'main', 'd.hash=')]


def enqueue_storage_check(daemon, hash_, digest, restore):
    # The insert hook's enqueue was refused: the daemon lives, Q and the claim
    # were not left behind, and the refusal is visible and classified. Once
    # storage is back, a later enqueue of the same torrent succeeds.
    assert daemon.call('system.pid') > 0
    assert daemon.call('d.custom', hash_, 'retrackers-queued') == '', 'Q kept without a durable save'
    assert daemon.call('system.retrackers.original_claim', '', hash_) == 'none'
    assert 'session-storage-failed' in daemon.call('d.message', hash_), daemon.call('d.message', hash_)
    log = (daemon.directory / 'native.log').read_text()
    assert 'RR enqueue refused: session-storage-failed: ' + hash_ in log, log[-2000:]
    daemon.call('d.message.set', hash_, '')  # Tracker success clears the volatile message.
    assert 'session-storage-failed' in daemon.call('d.message', hash_)
    assert daemon.call('d.custom', hash_, 'retrackers-enqueue-user') == digest
    restore()
    marker = daemon.call('d.retrackers.enqueue_original', hash_, daemon.call('d.local_id', hash_), digest,
                         str(daemon.call('d.state', hash_)))
    assert marker.startswith('v2:original:'), marker
    assert daemon.call('d.custom', hash_, 'retrackers-queued').startswith('q1:' + digest + ':')
    assert daemon.call('d.custom', hash_, 'retrackers-enqueue-refusal') == ''


def enqueue_hook(daemon, digest):
    # The retrackers insert hook's native call inside cat=, as plugins/retrackers
    # installs it (retrackersNativeEnqueueExpression).
    daemon.call('method.set_key', '', 'event.download.inserted_new', 'rr_enqueue_test',
                'cat="$d.retrackers.enqueue_original={$d.local_id=,' + digest + ',$cat=$d.state=}"')


def enqueue_storage_refusal(daemon):
    # A public torrent added while the session directory refuses writes
    # (EACCES): the hook's durable Q save fails, which once ended the daemon.
    digest = 'c' * 64
    other = daemon.fixture('storage-other')
    enqueue_hook(daemon, digest)
    daemon.session.chmod(0o2500)
    try:
        hash_ = daemon.fixture('storage-eacces')
        assert daemon.call('d.hash', other) == other
        enqueue_storage_check(daemon, hash_, digest, lambda: daemon.session.chmod(0o2700))
    finally:
        daemon.session.chmod(0o2700)  # Scratch cleanup needs a writable directory.


def enqueue_storage_full(daemon):
    # The same with a full session file system (ENOSPC): a small tmpfs.
    daemon.stop()
    daemon.options = ['--tmpfs', f'/data/.session:rw,size=2m,mode=2700,uid={os.getuid()},gid={os.getgid()}']
    daemon.start()
    digest = 'd' * 64
    other = daemon.fixture('full-other')
    enqueue_hook(daemon, digest)
    docker('exec', daemon.name, 'sh', '-c', 'cat /dev/zero > /data/.session/fill; true')
    hash_ = daemon.fixture('storage-enospc')
    assert daemon.call('d.hash', other) == other
    enqueue_storage_check(daemon, hash_, digest,
                          lambda: docker('exec', daemon.name, 'rm', '/data/.session/fill'))


def late_save_schedule(daemon):
    # A failed migration late save inside an asynchronous save is reported and
    # skipped: a scheduled session.save neither ends the daemon nor stops
    # saving the downloads after the failing one.
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    hash_ = daemon.fixture('late-save-claimed', start=False)
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    other = daemon.fixture('late-save-other', start=False)
    journal = daemon.session / '.retrackers-migration'
    prefix = hash_ + '.claim.' + tx
    receipt = journal / (prefix + '.late.receipt')
    sequence = int(receipt.read_text().split(':')[4]) + 1 if receipt.exists() else 1
    blocker = journal / (prefix + '.late.' + str(sequence) + '.a.torrent')
    blocker.mkdir(mode=0o700)
    daemon.call('d.custom.set', other, 'late-probe', 'saved-after-failure')
    daemon.call('schedule', '', 'rr_late_save_test', '1', '0', 'session.save=')
    sidecar = daemon.session / (other + '.torrent.rtorrent')
    daemon.wait(lambda: sidecar.exists() and b'saved-after-failure' in sidecar.read_bytes(),
                'the download after the failing one saved', timeout=15)
    assert daemon.call('system.pid') > 0
    log = (daemon.directory / 'native.log').read_text()
    assert 'RR late save ' + hash_ + ': ' in log, log[-2000:]
    blocker.rmdir()


def fixture_info(name):
    # The info dictionary Daemon.fixture() loads under this name.
    return {'length': 8, 'name': name, 'piece length': 16384,
            'pieces': hashlib.sha1(b'xxxxxxxx').digest()}


def same_hash_job(daemon, name, paused=False, worker=True, user='txuser', prior=False, crash=False):
    # A queued same-hash job as the retrackers plugin starts it, optionally
    # with a live native-owned worker (the fake php85 sleeping in update.php),
    # after an earlier completed job of the same torrent (prior), or with a
    # test cut ending the daemon inside the capture (crash).
    digest = dispatcher_actor(daemon, user) if worker else '0' * 64
    (daemon.directory / 'dl' / name).write_bytes(b'xxxxxxxx')  # Complete data: a seeding original.
    hash_ = daemon.fixture(name)
    daemon.wait(lambda: daemon.call('d.complete', hash_) == 1 and daemon.call('d.is_hash_checked', hash_) == 1,
                'source hash check complete')
    if paused:
        daemon.call('d.stop', hash_)
    assert [daemon.call(m, hash_) for m in ('d.state', 'd.is_open', 'd.is_active')] == (
        [0, 1, 0] if paused else [1, 1, 1])
    daemon.call('d.custom1.set', hash_, 'label-' + name)
    if prior:
        # A finished no-change job leaves an ordinary owner pointer behind.
        local = daemon.call('d.local_id', hash_)
        marker = daemon.call('d.retrackers.enqueue_original', hash_, local, digest,
                             str(daemon.call('d.state', hash_)))
        first, incarnation = marker.rsplit(':', 1)[1], daemon.call('d.incarnation', hash_)
        assert daemon.call('d.retrackers.finish_original', hash_, first, incarnation) == 'finalized'
        queue = daemon.call('d.custom', hash_, 'retrackers-queued')[68:]
        assert daemon.call('d.retrackers.dequeue_original', hash_, first, incarnation, queue) == 'dequeued'
        pointer = daemon.session / '.retrackers-migration' / (hash_ + '.owner.current')
        assert pointer.read_text().startswith('ordinary:'), pointer.read_text()
    local = daemon.call('d.local_id', hash_)
    marker = daemon.call('d.retrackers.enqueue_original', hash_, local, digest,
                         str(daemon.call('d.state', hash_)))
    tx, incarnation = marker.rsplit(':', 1)[1], daemon.call('d.incarnation', hash_)
    queue = daemon.call('d.custom', hash_, 'retrackers-queued')
    pid = None
    if worker:
        assert daemon.call('system.retrackers.worker_start', '', hash_, tx, incarnation, user) == 'spawned'
        pid = worker_pid(daemon, marker)
    # The candidate differs only outside the info dictionary, as a retracker
    # insertion does; capture binds its exact bytes.
    raw = encode({'announce': 'http://retracker.invalid/announce', 'info': fixture_info(name)})
    (daemon.directory / ('candidate-' + name + '.torrent')).write_bytes(raw)
    capture = lambda: daemon.call('d.retrackers.capture_erase', hash_, local, marker, hash_, tx,
                                  hashlib.sha256(raw).hexdigest())
    if crash:
        try:
            capture()
        except (OSError, RuntimeError):
            pass
        else:
            raise AssertionError('the capture cut did not end the daemon')
    else:
        capture()
        assert hash_ not in [r[0] for r in daemon.call('d.multicall2', '', 'main', 'd.hash=')]
    return {'hash': hash_, 'local': local, 'tx': tx, 'incarnation': incarnation, 'pid': pid,
            'name': name, 'paused': paused, 'queue': queue}


def worker_pid(daemon, marker):
    # The fake worker's pid from its actor.log line; its argv carries the marker.
    actors = daemon.directory / 'actor.log'
    def find():
        for line in actors.read_text().splitlines() if actors.exists() else []:
            if '/update.php ' in line and ' ' + marker + ' ' in line:
                return re.search(r' pid=(\d+)$', line).group(1)
    daemon.wait(find, 'worker launch')
    return find()


def same_hash_again(daemon, job, user):
    # The dispatcher's next pass for the same Q: a new claim, worker and capture.
    hash_ = job['hash']
    local = daemon.call('d.local_id', hash_)
    marker = daemon.call('d.retrackers.enqueue_original', hash_, local, hashlib.sha256(user.encode()).hexdigest(),
                         str(daemon.call('d.state', hash_)))
    tx, incarnation = marker.rsplit(':', 1)[1], daemon.call('d.incarnation', hash_)
    assert daemon.call('system.retrackers.worker_start', '', hash_, tx, incarnation, user) == 'spawned'
    pid = worker_pid(daemon, marker)
    raw = (daemon.directory / ('candidate-' + job['name'] + '.torrent')).read_bytes()
    daemon.call('d.retrackers.capture_erase', hash_, local, marker, hash_, tx, hashlib.sha256(raw).hexdigest())
    return dict(job, local=local, tx=tx, incarnation=incarnation, pid=pid)


def load_candidate(daemon, job):
    # The worker's candidate load: stopped, carrying the source's custom values
    # (its Q among them), applied and marked ready inside its creation scope,
    # the only place a same-hash candidate's markers change.
    hash_, tx = job['hash'], job['tx']
    daemon.call('load.normal', '', '/data/candidate-' + job['name'] + '.torrent',
                'd.custom.set=retrackers-recovery,v2:candidate-claim:' + tx,
                'd.custom.set=retrackers-queued,' + job['queue'],
                'd.custom1.set=label-' + job['name'],
                'd.retrackers.apply=' + hash_ + ',' + job['local'] + ',' + tx,
                'd.custom.set=retrackers-recovery,v2:candidate-ready:' + tx,
                'd.custom.set=retrackers-recovery-ack,v2:candidate-ready:' + tx)
    daemon.wait(lambda: daemon.call('d.hash', hash_) == hash_, 'candidate load')
    assert daemon.call('d.custom', hash_, 'retrackers-recovery-ack') == 'v2:candidate-ready:' + tx
    return daemon.call('d.incarnation', hash_)


def kill_worker(daemon, job):
    docker('exec', daemon.name, 'sh', '-c', 'kill -9 ' + job['pid'])


def owner_restored(daemon, job, completed, counted=0):
    # The original's run and open state and its label; its claim retired with
    # Q completed (commit: dequeue next) or owed (rollback: enqueue again).
    # A worker-exit rollback spends one counted attempt (capture-refused, due
    # 2^count seconds later); a commit or a startup restoration spends none.
    hash_ = job['hash']
    retry = daemon.call('d.custom', hash_, 'retrackers-retry')
    if counted:
        fields = retry.split(':')
        assert retry.startswith('r1:' + job['queue'][68:] + ':' + str(counted) + ':') and \
            fields[4] == 'capture-refused', retry
        assert -3 <= int(fields[3]) - time.time() <= (1 << counted) + 1, retry
    else:
        assert retry == '', 'retry changed: ' + retry
    state = [daemon.call(m, hash_) for m in ('d.state', 'd.is_open', 'd.is_active')]
    assert state == ([0, 1, 0] if job['paused'] else [1, 1, 1]), 'state/open/active ' + repr(state)
    assert daemon.call('d.custom1', hash_) == 'label-' + job['name']
    assert daemon.call('system.retrackers.status', '', hash_) == 'finalized'
    assert daemon.call('d.custom', hash_, 'retrackers-queued') == job['queue'], 'Q lost'
    claim = daemon.call('system.retrackers.original_claim', '', hash_)
    finished = daemon.call('d.custom', hash_, 'retrackers-queue-finished')
    if completed:
        assert claim.startswith('finalized:' + job['tx'] + ':'), claim
        assert finished.startswith('qd1:' + job['tx'] + ':'), finished
    else:
        assert claim == 'none' and finished == '', (claim, finished)


def recovered(daemon, job, outcome):
    # The classified line, the owner listed and no pending receipt of the dead
    # worker's job left for the plugin to read as an abandoned transaction.
    hash_ = job['hash']
    log = daemon.directory / 'native.log'
    line = 'RR worker exit recovery: ' + hash_ + ': ' + outcome
    daemon.wait(lambda: line in log.read_text(), 'recovery after the worker died', timeout=20)
    assert hash_ in [r[0] for r in daemon.call('d.multicall2', '', 'main', 'd.hash=')]
    keys = daemon.call('method.list_keys', '', 'rr.receipts.v1')
    assert 'wp:' + job['local'] not in keys and not [k for k in keys if k.endswith(':' + job['tx'])], keys


def worker_death_erased(daemon):
    # The worker died after capture erased the source and before the
    # candidate load: the daemon restores the original without a restart.
    job = same_hash_job(daemon, 'death-erased')
    kill_worker(daemon, job)
    recovered(daemon, job, 'rolled back; attempt 1 of 6')
    owner_restored(daemon, job, completed=False, counted=1)


def worker_death_candidate(daemon):
    # The worker died after the candidate load, before commit: the daemon
    # erases the uncommitted candidate and restores the paused original.
    job = same_hash_job(daemon, 'death-candidate', paused=True)
    load_candidate(daemon, job)
    kill_worker(daemon, job)
    recovered(daemon, job, 'rolled back; attempt 1 of 6')
    owner_restored(daemon, job, completed=False, counted=1)
    assert daemon.call('d.tracker_size', job['hash']) == 0, 'the uncommitted candidate stayed'


def worker_death_committed(daemon):
    # The worker died after commit: the daemon finishes the candidate.
    job = same_hash_job(daemon, 'death-committed')
    load_candidate(daemon, job)
    assert daemon.call('d.retrackers.commit', job['hash'], job['hash'], job['local'], job['tx']) == 'committed'
    kill_worker(daemon, job)
    recovered(daemon, job, 'finished')
    owner_restored(daemon, job, completed=True)
    assert daemon.call('d.tracker_size', job['hash']) == 1, 'the committed candidate was not kept'


def worker_death_restart(daemon):
    # The daemon died with its worker after the candidate load: startup
    # restores the original, and its saved started state really starts it.
    job = same_hash_job(daemon, 'death-restart')
    load_candidate(daemon, job)
    daemon.stop(kill=True)
    daemon.start()
    daemon.wait(lambda: job['hash'] in [r[0] for r in daemon.call('d.multicall2', '', 'main', 'd.hash=')],
                'startup restoration')
    daemon.wait(lambda: daemon.call('d.is_active', job['hash']) == 1, 'restored original started', timeout=10)
    owner_restored(daemon, job, completed=False)


def worker_death_loop(daemon):
    # A worker that always dies after capture: each rollback spends one counted
    # attempt with the application's backoff, the gate waits for it, and the
    # sixth makes the job terminal, so no seventh capture stops the torrent.
    user = 'loopuser'
    job = same_hash_job(daemon, 'death-loop', user=user)
    hash_, queue = job['hash'], job['queue'][68:]
    dispatch = lambda: daemon.call('system.retrackers.dispatcher_start', '', user)
    for attempt in range(1, 7):
        if attempt > 1:
            job = same_hash_again(daemon, job, user)
        kill_worker(daemon, job)
        recovered(daemon, job, 'rolled back; attempt %d of 6' % attempt)
        assert [daemon.call(m, hash_) for m in ('d.state', 'd.is_open', 'd.is_active')] == [1, 1, 1]
        retry = daemon.call('d.custom', hash_, 'retrackers-retry')
        if attempt == 6:
            assert retry == f'r1:{queue}:6:0:capture-refused', retry
            break
        assert retry.startswith(f'r1:{queue}:{attempt}:') and retry.endswith(':capture-refused'), retry
        due = int(retry.split(':')[3])
        if time.time() < due - .5:
            assert dispatch() == 'idle', f'attempt {attempt}: pass launched before the counted retry was due'
        while time.time() <= due:
            time.sleep(.2)
        daemon.wait(lambda: dispatch() == 'spawned', f'attempt {attempt}: pass after the due time', timeout=5)
    log = (daemon.directory / 'native.log').read_text()
    assert 'rolled back; attempt 6 of 6, retries exhausted' in log, log[-2000:]
    assert 'retries exhausted' in daemon.call('d.message', hash_)
    # Terminal: the gate launches nothing more and the torrent keeps seeding.
    for _ in range(3):
        assert dispatch() == 'idle', 'terminal job dispatched again'
        time.sleep(1)
    assert [daemon.call(m, hash_) for m in ('d.state', 'd.is_open', 'd.is_active')] == [1, 1, 1]
    assert log.count('RR worker exit recovery: ' + hash_) == 6


def worker_death_partial(daemon):
    # A worker that dies with a partial capsule leaves its claim held for
    # startup: the daemon says so instead of waiting mutely.
    user = 'partialuser'
    digest = dispatcher_actor(daemon, user)
    hash_ = daemon.fixture('death-partial')
    marker = daemon.call('d.retrackers.enqueue_original', hash_, daemon.call('d.local_id', hash_), digest, '1')
    tx, incarnation = marker.rsplit(':', 1)[1], daemon.call('d.incarnation', hash_)
    assert daemon.call('system.retrackers.worker_start', '', hash_, tx, incarnation, user) == 'spawned'
    partial = daemon.session / '.retrackers-migration' / (hash_ + '.rollback.torrent')
    partial.write_bytes(b'partial captured snapshot')
    partial.chmod(0o600)
    actors = daemon.directory / 'actor.log'
    daemon.wait(lambda: actors.exists() and '/update.php ' + hash_ in actors.read_text(), 'worker launch')
    pid = re.search(r' pid=(\d+)$', [line for line in actors.read_text().splitlines()
                                      if '/update.php ' + hash_ in line][0]).group(1)
    docker('exec', daemon.name, 'sh', '-c', 'kill -9 ' + pid)
    log = daemon.directory / 'native.log'
    line = 'RR worker exit recovery held: ' + hash_ + ': partial capture publication'
    daemon.wait(lambda: line in log.read_text(), 'a visible hold', timeout=20)
    assert daemon.call('d.hash', hash_) == hash_
    assert daemon.call('system.retrackers.original_claim', '', hash_) == 'held'


def worker_death_reprocessed(daemon):
    # The same after an earlier completed job left an ordinary owner pointer:
    # the uncommitted candidate is no owner, so its erase leaves the pointer.
    job = same_hash_job(daemon, 'death-reprocessed', prior=True)
    load_candidate(daemon, job)
    kill_worker(daemon, job)
    recovered(daemon, job, 'rolled back; attempt 1 of 6')
    owner_restored(daemon, job, completed=False, counted=1)


def capture_phase(daemon, phase, stop):
    # A stop or crash at one phase of a same-hash capture: shutdown neither
    # crashes nor loses another torrent's unsaved change, and after one start
    # the original has its prior state, or the committed candidate is active.
    others = []
    for index in range(6):
        name = 'phase-other-' + str(index)
        (daemon.directory / 'dl' / name).write_bytes(b'xxxxxxxx')
        others.append(daemon.fixture(name))
    crash = phase in ('intent', 'quiesced')
    job = same_hash_job(daemon, 'phase-' + phase, crash=crash)
    if phase in ('candidate', 'committed'):
        load_candidate(daemon, job)
    if phase == 'committed':
        assert daemon.call('d.retrackers.commit', job['hash'], job['hash'], job['local'], job['tx']) == 'committed'
    if stop:
        for index, hash_ in enumerate(others):
            daemon.call('d.custom.set', hash_, 'phase-mark', 'unsaved-' + str(index))
        daemon.stop()  # Asserts a clean exit.
    elif crash:
        docker('rm', '-f', daemon.name)  # The test cut already ended the daemon.
    else:
        daemon.stop(kill=True)
    daemon.cut = ''
    daemon.start()
    if stop:
        marks = [daemon.call('d.custom', hash_, 'phase-mark') for hash_ in others]
        assert marks == ['unsaved-' + str(index) for index in range(6)], 'shutdown save lost: ' + repr(marks)
    hash_ = job['hash']
    daemon.wait(lambda: hash_ in [r[0] for r in daemon.call('d.multicall2', '', 'main', 'd.hash=')],
                'restoration after start')
    daemon.wait(lambda: daemon.call('d.is_active', hash_) == 1, 'restored owner started', timeout=15)
    owner_restored(daemon, job, completed=phase == 'committed')
    assert daemon.call('d.tracker_size', hash_) == (1 if phase == 'committed' else 0)


def retry_cleared_dequeue(daemon):
    # A job that waited and then completed leaves no retry receipt behind.
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    hash_ = daemon.fixture('retry-cleared', start=False)
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    wait = f'r1:{queue}:1:{int(time.time()) + 60}:source-not-ready'
    assert daemon.call('d.retrackers.defer_original', hash_, tx, incarnation, queue, '', wait) == 'cancelled'
    marker = daemon.call('d.retrackers.enqueue_original', hash_, daemon.call('d.local_id', hash_), '0' * 64, '0')
    tx = marker.rsplit(':', 1)[1]
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == wait
    assert daemon.call('d.retrackers.finish_original', hash_, tx, incarnation) == 'finalized'
    assert daemon.call('d.retrackers.dequeue_original', hash_, tx, incarnation, queue) == 'dequeued'
    assert daemon.call('d.custom', hash_, 'retrackers-queued') == ''
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == '', 'retry kept after completion'
    daemon.stop()
    daemon.start()
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == ''





def immediate_shutdown(daemon):
    # Latest upstream NetworkConfig owns a delayed notification. Shutdown
    # must drain it, including a new mutation immediately before SIGINT.
    daemon.call('network.block.ipv6.set', '', 0)
    daemon.stop()
    daemon.start()
    daemon.call('network.block.ipv6.set', '', 1)
    docker('kill', '--signal', 'SIGINT', daemon.name)
    daemon.wait(lambda: docker('inspect', '-f', '{{.State.Running}}', daemon.name).stdout.strip() == 'false', 'immediate normal shutdown')
    assert docker('inspect', '-f', '{{.State.ExitCode}}', daemon.name).stdout.strip() == '0'
    docker('rm', daemon.name)
    daemon.start()



def move_sync_failure(daemon):
    name = 'move-sync-failure'
    (daemon.directory / 'dl' / name).write_bytes(b'xxxxxxxx')
    h = daemon.fixture(name)
    daemon.wait(lambda: daemon.call('d.complete', h) == 1, 'Move sync complete')
    daemon.call('d.save_full_session', h)
    daemon.wait(lambda: b'8:completei1e' in (daemon.session / (h + '.torrent.rtorrent')).read_bytes(),
                'complete session before sync failure')
    enqueue = lambda: daemon.call('d.autotools.move_enqueue', h, 'moveuser')
    expect_fault(enqueue, 'enqueue refused')
    record = daemon.session / '.rutorrent-stop-intents' / (h + '.move')
    original = record.read_bytes()
    q = original.decode().split(':')[2]
    # A visible record after failed directory sync is still not durable
    # admission; idempotent retry must pass the same sync boundary.
    expect_fault(enqueue, 'enqueue refused')
    assert record.read_bytes() == original
    daemon.call('d.message.set', h, '')
    assert 'session-storage-failed' in daemon.call('d.message', h)
    daemon.stop(kill=True)
    daemon.cut = ''
    daemon.start()
    assert enqueue() == 'exists:' + q
    assert daemon.call('d.message', h) == ''
    assert record.read_bytes() == original


def move_publication_crash(daemon, after=False, finish=False):
    name = 'move-publication'
    (daemon.directory / 'dl' / name).write_bytes(b'xxxxxxxx')
    h = daemon.fixture(name)
    daemon.wait(lambda: daemon.call('d.complete', h) == 1, 'Move publication complete')
    daemon.call('d.save_full_session', h)
    daemon.wait(lambda: b'8:completei1e' in (daemon.session / (h + '.torrent.rtorrent')).read_bytes(),
                'complete session before record publication cut')
    q = daemon.call('d.autotools.move_enqueue', h, 'moveuser')[7:] if finish else None
    try:
        if finish:
            daemon.call('d.autotools.move_finish', h, q)
        else:
            daemon.call('d.autotools.move_enqueue', h, 'moveuser')
    except (OSError, RuntimeError):
        pass
    else:
        raise AssertionError('Move publication cut not reached')
    daemon.wait(lambda: docker('inspect', '-f', '{{.State.Running}}', daemon.name).stdout.strip() == 'false', 'record cut exit')
    docker('rm', daemon.name)
    record = daemon.session / '.rutorrent-stop-intents' / (h + '.move')
    assert record.exists() == (after and not finish)
    daemon.cut = ''
    daemon.start()
    state = daemon.call('system.autotools.move_state', '', h)
    assert state.startswith('queued:') if after and not finish else state == 'none'
    assert not list(record.parent.glob(h + '.move.*.tmp'))
    assert (daemon.directory / 'dl' / name).read_bytes() == b'xxxxxxxx'


def move_claim_crash(daemon, supersede=False):
    h, q = move_complete(daemon, 'move-claim-cut')
    daemon.call('d.save_full_session', h)
    daemon.wait(lambda: b'8:completei1e' in (daemon.session / (h + '.torrent.rtorrent')).read_bytes(), 'claim source save')
    job = 'e' * 32 if supersede else q
    try:
        if supersede:
            daemon.call('d.stop_close_claim', h, job)
        else:
            daemon.call('d.autotools.move_begin', h, q, daemon.call('d.incarnation', h))
    except (OSError, RuntimeError):
        pass
    else:
        raise AssertionError('Move claim cut not reached')
    daemon.wait(lambda: docker('inspect', '-f', '{{.State.Running}}', daemon.name).stdout.strip() == 'false', 'claim cut exit')
    token = (daemon.session / '.rutorrent-stop-intents' / (h + '.claim')).read_text().splitlines()[0]
    docker('rm', daemon.name)
    daemon.cut = ''
    daemon.start()
    assert daemon.call('d.stop_close_claim_held', h) == token
    if supersede:
        assert daemon.call('system.autotools.move_dispatch', '', 'moveuser') == 'idle'
        assert daemon.call('system.autotools.move_state', '', h) == 'none'
        daemon.call('d.message.set', h, '')
        assert 'superseded-by-user' in daemon.call('d.message', h)
    else:
        result = daemon.call('d.autotools.move_begin', h, q, daemon.call('d.incarnation', h))
        assert result.startswith('active|' + token + '|')
        assert [daemon.call(method, h) for method in ('d.state', 'd.is_open', 'd.is_active')] == [0, 0, 0]
    assert (daemon.directory / 'dl' / 'move-claim-cut').read_bytes() == b'xxxxxxxx'


def move_no_adopter(daemon):
    # Production-image compatible: no compiler, replacement actor or test cut.
    control = daemon.fixture('move-no-adopter-control', start=False)
    pid = daemon.call('system.pid')
    methods = set(daemon.call('system.listMethods'))
    assert 'd.autotools.move_adopt' not in methods, 'removed Move adopter remains registered'
    required = {'system.autotools.move_queue', 'system.autotools.move_state',
                'system.autotools.move_owner', 'system.autotools.move_dispatch',
                'system.autotools.move_reset', 'd.autotools.move_enqueue',
                'd.autotools.move_begin', 'd.autotools.move_defer',
                'd.autotools.move_finish', 'd.autotools.move_cancel'}
    assert required <= methods, 'current Move queue capability was removed'
    assert daemon.call('system.autotools.move_queue', '') == 1
    try:
        daemon.call('d.autotools.move_adopt', control, 'a' * 32, 'moveuser')
    except xmlrpc.client.Fault as error:
        assert error.faultCode == -506, 'removed method failed for a reason other than absence'
    else:
        raise AssertionError('removed Move adopter still callable')
    assert daemon.call('system.pid') == pid
    assert daemon.call('d.hash', control) == control
    assert daemon.call('d.state', control) == 0


def move_complete(daemon, name, user='moveuser'):
    (daemon.directory / 'dl' / name).write_bytes(b'xxxxxxxx')
    hash_ = daemon.fixture(name)
    daemon.wait(lambda: daemon.call('d.complete', hash_) == 1, 'complete Move source')
    queued = daemon.call('d.autotools.move_enqueue', hash_, user)
    assert queued.startswith('queued:'), queued
    return hash_, queued[7:]


def move_queue(daemon):
    assert daemon.call('system.autotools.move_queue', '') == 1
    incomplete = daemon.fixture('move-incomplete', start=False)
    expect_fault(lambda: daemon.call('d.autotools.move_enqueue', incomplete, 'moveuser'), 'complete')
    hash_, q = move_complete(daemon, 'move-queue')
    state = lambda: daemon.call('system.autotools.move_state', '', hash_)
    assert state().startswith('queued:' + hashlib.sha256(b'moveuser').hexdigest() + ':' + q + ':')
    assert daemon.call('d.autotools.move_enqueue', hash_, 'moveuser') == 'exists:' + q
    record = daemon.session / '.rutorrent-stop-intents' / (hash_ + '.move')
    saved = record.read_bytes()
    expect_fault(lambda: daemon.call('d.autotools.move_enqueue', hash_, 'foreignuser'), 'owner-changed')
    assert record.read_bytes() == saved
    assert daemon.call('d.custom', hash_, 'autotools-enqueue-refusal') == ''
    assert 'owner-changed' in daemon.call('d.message', hash_)
    daemon.call('d.message.set', hash_, '')
    assert record.stat().st_mode & 0o777 == 0o600
    daemon.stop()
    daemon.start()
    assert record.read_bytes() == saved
    assert state().startswith('queued:')
    i = daemon.call('d.incarnation', hash_)
    assert daemon.call('d.autotools.move_begin', hash_, q, 'f' * 32) == 'busy:incarnation-changed'
    begin = daemon.call('d.autotools.move_begin', hash_, q, i)
    assert begin.startswith('active|'), begin
    token = begin.split('|')[1]
    assert daemon.call('d.stop_close_claim_held', hash_) == token
    assert state().endswith(':' + token)
    expect_fault(lambda: daemon.call('d.autotools.move_finish', hash_, q), 'blocked by lease')
    expect_fault(lambda: daemon.call('d.erase', hash_), 'reserved')
    daemon.call('d.stop', hash_)  # A late user Stop overrides the original run receipt.
    daemon.call('d.start_if_stop_close_claim', hash_, token)
    daemon.call('d.replay_stop_close_claim', hash_, q)
    daemon.call('d.ack_stop_close_claim', hash_, q)
    assert daemon.call('d.state', hash_) == 0
    previous = ''
    wait = f'r1:{q}:0:{int(time.time()) + 30}:source-not-ready'
    assert daemon.call('d.autotools.move_defer', hash_, q, previous, wait) == 'deferred'
    previous = wait
    expect_fault(lambda: daemon.call('d.autotools.move_defer', hash_, q, '', wait), 'CAS changed')
    for count in range(1, 7):
        due = 0 if count == 6 else int(time.time()) + 60 * 2 ** (count - 1)
        retry = f'r1:{q}:{count}:{due}:preflight-refused'
        assert daemon.call('d.autotools.move_defer', hash_, q, previous, retry) == 'deferred'
        previous = retry
    assert state().startswith('terminal:')
    daemon.call('d.message.set', hash_, '')
    assert 'retries exhausted' in daemon.call('d.message', hash_)
    assert daemon.call('system.autotools.move_dispatch', '', 'moveuser') == 'idle'
    daemon.stop()
    daemon.start()
    assert state().startswith('terminal:')
    assert daemon.call('system.autotools.move_reset', '', hash_) == 'reset'
    assert state().endswith(':')
    expect_fault(lambda: daemon.call('d.autotools.move_finish', hash_, 'a' * 32), 'queue changed')
    assert daemon.call('d.autotools.move_finish', hash_, q) == 'finished'
    assert state() == 'none'
    assert not record.exists()
    assert daemon.call('d.autotools.move_finish', hash_, q) == 'none'


def move_supersede(daemon):
    hash_, q = move_complete(daemon, 'move-supersede')
    job = 'e' * 32
    receipt = daemon.call('d.stop_close_claim', hash_, job)
    assert daemon.call('system.autotools.move_state', '', hash_) == 'none'
    assert 'superseded-by-user' in daemon.call('d.message', hash_)
    daemon.call('d.release_stop_close_claim', hash_, receipt.split('|')[0])
    daemon.call('d.replay_stop_close_claim', hash_, job)
    daemon.call('d.ack_stop_close_claim', hash_, job)
    hash_, q = move_complete(daemon, 'move-direct-erase')
    record = daemon.session / '.rutorrent-stop-intents' / (hash_ + '.move')
    daemon.call('d.erase', hash_)
    assert not record.exists()



def move_pool(daemon):
    user = 'moveuser'
    digest = dispatcher_actor(daemon, user)
    scripts = daemon.directory / 'app' / 'plugins' / 'autotools'
    scripts.mkdir(mode=0o755)
    (scripts / 'move_worker.php').write_text('<?php\n')
    (scripts / 'move_worker.php').chmod(0o644)
    jobs = [move_complete(daemon, 'pool-move-' + str(n), user) for n in range(3)]
    dispatch = lambda: daemon.call('system.autotools.move_dispatch', '', user)
    assert dispatch() == 'spawned'
    assert dispatch() == 'spawned'
    assert dispatch() == 'full', 'Move exceeded its two shared reservations'
    # Six retracker workers can use the remaining slots; the seventh cannot.
    for n in range(7):
        h = daemon.fixture('pool-rr-' + str(n), start=False)
        marker = daemon.call('d.retrackers.enqueue_original', h, daemon.call('d.local_id', h), digest, '0')
        result = daemon.call('system.retrackers.worker_start', '', h, marker.rsplit(':', 1)[1], daemon.call('d.incarnation', h), user)
        assert result == ('spawned' if n < 6 else 'full'), result
    actors = daemon.directory / 'actor.log'
    daemon.wait(lambda: len([line for line in actors.read_text().splitlines() if '/move_worker.php ' in line]) == 2,
                'two fixed Move actors')
    lines = [line for line in actors.read_text().splitlines() if '/move_worker.php ' in line]
    line = lines[0].split()
    h, q = line[2], line[3]
    pid = line[-1].split('=')[1]
    record = daemon.session / '.rutorrent-stop-intents' / (h + '.move')
    docker('exec', daemon.name, 'kill', '-9', pid)
    daemon.wait(lambda: b':1:' in record.read_bytes() and b':worker-exit' in record.read_bytes(), 'counted Move exit')
    state = daemon.call('system.autotools.move_state', '', h)
    assert state.endswith(':worker-exit'), state
    assert f'r1:{q}:1:' in state
    assert dispatch() == 'spawned', 'reaped shared reservation did not become available'



def move_worker_exhaustion(daemon):
    user = 'moveuser'
    dispatcher_actor(daemon, user)
    scripts = daemon.directory / 'app' / 'plugins' / 'autotools'
    scripts.mkdir(mode=0o755)
    (scripts / 'move_worker.php').write_text('<?php\n')
    (scripts / 'move_worker.php').chmod(0o644)
    h, q = move_complete(daemon, 'move-exhausted', user)
    i = daemon.call('d.incarnation', h)
    receipt = daemon.call('d.autotools.move_begin', h, q, i)
    assert receipt.startswith('active|')
    token = receipt.split('|')[1]
    record = daemon.session / '.rutorrent-stop-intents' / (h + '.move')
    actors = daemon.directory / 'actor.log'
    dispatch = lambda: daemon.call('system.autotools.move_dispatch', '', user)
    for count in range(1, 7):
        if count > 1:
            fields = record.read_text().strip().split(':')
            fields[8] = '1'  # Advance only the fixture's clock eligibility; keep Q/count/reason.
            record.write_text(':'.join(fields) + '\n')
        before = actors.read_text().splitlines() if actors.exists() else []
        assert dispatch() == 'spawned'
        daemon.wait(lambda: len(actors.read_text().splitlines()) > len(before), 'Move actor launch')
        pid = actors.read_text().splitlines()[-1].split(' pid=')[1]
        docker('exec', daemon.name, 'kill', '-9', pid)
        assert dispatch() == 'idle', 'reaped identity relaunched before exit accounting/backoff'
        daemon.wait(lambda: len(record.read_text().strip().split(':')) == 10 and
                    record.read_text().strip().split(':')[7] == str(count), 'Move worker exit counted')
        fields = record.read_text().strip().split(':')
        assert fields[9] == 'worker-exit'
        due = int(fields[8])
        assert due == 0 if count == 6 else 0 < due - int(time.time()) <= 60 * 2 ** (count - 1)
        assert daemon.call('d.stop_close_claim_held', h) == token, 'exit retired captured lease'
    assert daemon.call('system.autotools.move_state', '', h) == 'held:worker-exit'
    assert daemon.call('system.autotools.move_owner', '', h) == hashlib.sha256(user.encode()).hexdigest()
    assert dispatch() == 'idle'
    assert 'retries exhausted' in daemon.call('d.message', h)
    assert daemon.call('system.autotools.move_reset', '', h) == 'reset'
    assert daemon.call('d.stop_close_claim_held', h) == token
    assert dispatch() == 'spawned', 'reset did not resume the retained recovery'


def move_capture_restart(daemon):
    name = 'move-capture-restart'
    h, q = move_complete(daemon, name)
    record = daemon.session / '.rutorrent-stop-intents' / (h + '.move')
    saved = record.read_bytes()
    job = same_hash_job(daemon, name, worker=False)
    assert record.read_bytes() == saved, 'capture erase lost native Move intent'
    daemon.stop(kill=True)
    daemon.start()
    daemon.wait(lambda: daemon.call('d.hash', h) == h, 'captured original restored')
    assert record.read_bytes() == saved
    assert daemon.call('d.custom', h, 'retrackers-queued') == job['queue']
    assert daemon.call('d.custom', h, 'retrackers-queue-finished') == ''
    assert daemon.call('system.autotools.move_state', '', h).startswith('queued:')


def move_storage(daemon):
    # A failed record publication must be visible after the volatile message
    # is cleared, without terminating an event hook or touching payload.
    h, q = move_complete(daemon, 'move-storage')
    assert daemon.call('d.autotools.move_finish', h, q) == 'finished'
    intent = daemon.session / '.rutorrent-stop-intents'
    intent.chmod(0o500)
    try:
        expect_fault(lambda: daemon.call('d.autotools.move_enqueue', h, 'moveuser'), 'record-unreadable')
        daemon.call('d.message.set', h, '')
        assert 'record-malformed' in daemon.call('d.message', h)
        assert daemon.call('system.pid') > 0
    finally:
        intent.chmod(0o700)
    assert daemon.call('d.autotools.move_enqueue', h, 'moveuser').startswith('queued:')
    assert daemon.call('d.message', h) == ''
    intent.chmod(0o500)
    try:
        expect_fault(lambda: daemon.call('d.autotools.move_enqueue', h, 'moveuser'), 'record-unreadable')
    finally:
        intent.chmod(0o700)
    assert daemon.call('d.autotools.move_enqueue', h, 'moveuser').startswith('exists:')
    assert daemon.call('d.message', h) == '', 'repaired idempotent enqueue retained owned refusal'



def move_foreign_leaf(daemon):
    # Another Unix UID writes the leaf while the owner has temporarily made
    # its parents writable. Restoring parent permissions does not make that
    # foreign single-link regular record admissible.
    h, q = move_complete(daemon, 'move-foreign-leaf')
    other = daemon.fixture('move-foreign-control', start=False)
    intent = daemon.session / '.rutorrent-stop-intents'
    record = intent / (h + '.move')
    original = record.read_bytes()
    record.unlink()
    daemon.session.chmod(0o777)
    intent.chmod(0o777)
    try:
        docker('exec', '--user', '65534:65534', daemon.name, 'sh', '-c',
               'umask 077; printf %s "$1" > "$2"', 'sh', original.decode(),
               '/data/.session/.rutorrent-stop-intents/' + h + '.move')
    finally:
        intent.chmod(0o700)
        daemon.session.chmod(0o2700)
    assert record.stat().st_uid == 65534
    assert daemon.call('system.autotools.move_state', '', h) == 'held:record-malformed'
    expect_fault(lambda: daemon.call('d.autotools.move_begin', h, q, daemon.call('d.incarnation', h)), 'record-unreadable')
    assert daemon.call('d.hash', other) == other
    assert daemon.call('system.pid') > 0
    record.unlink()
    record.write_bytes(original)
    record.chmod(0o600)
    assert daemon.call('d.autotools.move_cancel', h, q, 'configuration-declined') == 'cancelled'


def move_corrupt(daemon):
    hash_, q = move_complete(daemon, 'move-corrupt')
    record = daemon.session / '.rutorrent-stop-intents' / (hash_ + '.move')
    original = record.read_bytes()
    record.write_bytes(b'm1:broken\n')
    assert daemon.call('system.autotools.move_state', '', hash_) == 'held:record-malformed'
    daemon.call('d.message.set', hash_, '')
    assert str(hash_) + '.move' in daemon.call('d.message', hash_)
    expect_fault(lambda: daemon.call('d.erase', hash_), 'Move record blocks')
    assert daemon.call('system.pid') > 0
    record.write_bytes(original)
    assert daemon.call('d.autotools.move_cancel', hash_, q, 'configuration-declined') == 'cancelled'


def samehash_prepared_prior(daemon, foreign=False):
    # ACC4-OWN-N-01: a previous ordinary owner pointer belongs to the source,
    # while prepared belongs to its committed same-hash successor.
    daemon.stop(kill=True)
    daemon.cut = ''
    daemon.start()
    unrelated = daemon.fixture('prepared-unrelated', start=False)
    daemon.call('d.save_full_session', unrelated)
    daemon.wait(lambda: (daemon.session / (unrelated + '.torrent')).exists(), 'unrelated session save')
    name = 'prepared-prior'
    (daemon.directory / 'dl' / name).write_bytes(b'xxxxxxxx')
    hash_ = daemon.fixture(name)
    daemon.wait(lambda: daemon.call('d.is_hash_checked', hash_) == 1, 'initial hash check')
    tx, incarnation = daemon.begin(hash_)
    assert daemon.call('d.retrackers.finish_original', hash_, tx, incarnation) == 'finalized'
    pointer = daemon.session / '.retrackers-migration' / (hash_ + '.owner.current')
    daemon.stop(kill=True)
    daemon.cut = 'after-samehash-prepared'
    daemon.start()
    job = same_hash_job(daemon, name, worker=False)
    candidate = load_candidate(daemon, job)
    assert candidate != incarnation
    assert daemon.call('d.retrackers.commit', hash_, hash_, job['local'], job['tx']) == 'committed'
    try:
        daemon.call('d.retrackers.release_committed', hash_, job['tx'], candidate)
    except (OSError, RuntimeError):
        pass
    else:
        raise AssertionError('samehash prepare cut did not end the daemon')
    daemon.wait(lambda: docker('inspect', '-f', '{{.State.Running}}', daemon.name).stdout.strip() == 'false', 'prepare cut exit')
    assert pointer.read_text().strip().split(':')[-1] == incarnation, 'cut must retain source owner incarnation'
    assert pointer.read_text().strip().split(':')[-1] != candidate
    assert (daemon.session / '.retrackers-migration' / (hash_ + '.prepared.receipt')).exists()
    docker('rm', daemon.name)
    if foreign:
        pointer.write_text('retired:' + hash_ + ':' + 'f' * 32 + '\n')
    daemon.cut = ''
    daemon.start()
    assert daemon.call('d.hash', unrelated) == unrelated
    if foreign:
        assert hash_ not in [row[0] for row in daemon.call('d.multicall2', '', 'main', 'd.hash=')]
        assert 'RR prepared startup authority mismatch' in (daemon.directory / 'native.log').read_text()
        assert (daemon.directory / 'dl' / name).read_bytes() == b'xxxxxxxx'
        return
    daemon.wait(lambda: daemon.call('d.hash', hash_) == hash_, 'samehash prepared source returns')
    assert daemon.call('system.retrackers.original_claim', '', hash_).startswith('finalized:' + job['tx'] + ':')
    assert daemon.call('d.incarnation', hash_) == candidate
    assert (daemon.directory / 'dl' / name).read_bytes() == b'xxxxxxxx'
    assert daemon.call('d.custom', hash_, 'retrackers-queued') == job['queue']
    assert daemon.call('d.custom', hash_, 'retrackers-queue-finished').endswith(job['queue'][68:])
    daemon.stop(kill=True)
    daemon.start()
    assert daemon.call('d.hash', hash_) == hash_



def samehash_resume(daemon):
    # ACC4-SCALE-01 / ACC4-OWN-P4c: libtorrent logs each full hash pass.
    # Copying the captured resume proof must keep both active and paused
    # replacements on its normal quick-check path (sizes/mtimes still apply).
    for paused in (False, True):
        job = same_hash_job(daemon, 'resume-paused-' + str(int(paused)), paused=paused, worker=False)
        h = job['hash']
        log = daemon.directory / 'native.log'
        passes = lambda: len([line for line in log.read_text().splitlines() if h in line and 'try_quick:0.' in line])
        initial = passes()
        assert initial == 1, ('initial full hash passes', initial)
        candidate = load_candidate(daemon, job)
        assert daemon.call('d.retrackers.commit', h, h, job['local'], job['tx']) == 'committed'
        assert daemon.call('d.retrackers.release_committed', h, job['tx'], candidate) == 'released'
        if paused:
            assert daemon.call('d.complete', h) == 1, 'paused completion bit lost'
            daemon.call('d.start', h)
        daemon.wait(lambda: daemon.call('d.is_active', h) == 1 and daemon.call('d.is_hash_checked', h) == 1,
                    'samehash replacement resumed')
        assert passes() == initial, 'samehash replacement hashed payload twice'
        assert (daemon.directory / 'dl' / job['name']).read_bytes() == b'xxxxxxxx'


def pause_kept(daemon):
    # A paused (open, inactive) original stays paused through a completed
    # same-hash transaction: the release reopens what capture closed.
    job = same_hash_job(daemon, 'pause-kept', paused=True, worker=False)
    incarnation = load_candidate(daemon, job)
    assert daemon.call('d.retrackers.commit', job['hash'], job['hash'], job['local'], job['tx']) == 'committed'
    assert daemon.call('d.retrackers.release_committed', job['hash'], job['tx'], incarnation) == 'released'
    owner_restored(daemon, job, completed=True)


def erase_retire_batch(daemon):
    # Erases queue their tombstones for one batch a few seconds later instead
    # of three directory syncs each. A clean shutdown runs a pending batch; a
    # crash before it leaves the tombstone to startup without resurrecting
    # anything.
    journal = daemon.session / '.retrackers-migration'
    hashes = []
    for index in range(7):
        hash_ = daemon.fixture('retire-batch-' + str(index))
        tx, incarnation = daemon.begin(hash_)
        assert daemon.call('d.retrackers.finish_original', hash_, tx, incarnation) == 'finalized'
        hashes.append(hash_)
    owned = lambda hash_: sorted(x.name[41:] for x in journal.iterdir() if x.name.startswith(hash_))
    for hash_ in hashes[:5]:
        daemon.call('d.erase', hash_)
    assert all(owned(h) == ['finalized.receipt', 'owner.current'] for h in hashes[:5]), 'not deferred'
    daemon.wait(lambda: not any(owned(h) for h in hashes[:5]), 'one deferred retirement batch', timeout=10)
    erased = time.time()
    daemon.call('d.erase', hashes[5])
    assert owned(hashes[5]) == ['finalized.receipt', 'owner.current'], 'not deferred'
    result = docker('stop', '-t', '60', daemon.name)
    state = docker('inspect', '-f', '{{.State.ExitCode}} {{.State.FinishedAt}}', daemon.name).stdout.split()
    docker('rm', daemon.name)
    assert state[0] == '0', (state, result)
    # The batch timer waits at least two seconds from the main loop's cached
    # time at the erase, so an exit well within them proves that the shutdown
    # itself retired the tombstone.
    stamp = re.fullmatch(r'(\S+?)(?:\.(\d+))?Z', state[1])
    finished = datetime.datetime.strptime(stamp.group(1), '%Y-%m-%dT%H:%M:%S').replace(
        tzinfo=datetime.timezone.utc).timestamp() + float('0.' + (stamp.group(2) or '0'))
    print(f'  clean exit {finished - erased:.2f} s after the erase', flush=True)
    assert finished - erased < 1.9, f'the daemon took {finished - erased:.2f} s to exit after the erase'
    assert not owned(hashes[5]), 'clean shutdown kept the pending tombstone: ' + repr(owned(hashes[5]))
    daemon.start()
    daemon.call('d.erase', hashes[6])
    daemon.stop(kill=True)
    assert owned(hashes[6]) == ['finalized.receipt', 'owner.current']
    daemon.start()
    assert not daemon.call('d.multicall2', '', 'main', 'd.hash='), 'erased owner resurrected'
    assert not owned(hashes[6]), 'startup kept the pending tombstone: ' + repr(owned(hashes[6]))


def dispatcher_actor(daemon, user):
    # The fake php85 actor and a single-user ledger profile, as dispatcher_gate.
    compile_fixture(daemon, 'fake-php.cc', 'fake-php')
    scripts = daemon.directory / 'app' / 'plugins' / 'retrackers'
    scripts.mkdir(parents=True)
    for path in (daemon.directory, daemon.directory / 'app', scripts.parent, scripts):
        path.chmod(0o755)
    for script in ('dispatch.php', 'update.php'):
        (scripts / script).write_text('<?php\n')
        (scripts / script).chmod(0o644)
    (daemon.directory / 'fake-php').chmod(0o755)
    daemon.stop()
    daemon.environment = {'RUTORRENT_NATIVE_PHP': '/data/fake-php', 'RUTORRENT_NATIVE_APP_ROOT': '/data/app'}
    daemon.start()
    digest = hashlib.sha256(user.encode()).hexdigest()
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    daemon.call('method.set_key', '', 'rr.receipts.v1', 'pv:' + 'a' * 32, '1')
    daemon.call('method.set_key', '', 'rr.receipts.v1', 'pf:' + digest + ':' + 'b' * 64, '1')
    return digest


def dispatcher_gate_busy(daemon):
    # Work that a pass cannot do now launches nothing: a none claim under a
    # file-operation stop lease, and worker jobs while all eight slots run.
    # Dequeueing a finalized claim needs no slot and still launches.
    user = 'busyuser'
    digest = dispatcher_actor(daemon, user)
    dispatch = lambda: daemon.call('system.retrackers.dispatcher_start', '', user)
    leased = daemon.fixture('busy-lease', start=False)
    queue = uuid.uuid4().hex
    daemon.call('d.custom.set', leased, 'retrackers-queued', 'q1:' + digest + ':' + queue)
    daemon.call('d.stop_close_claim', leased, 'b' * 32)
    expect_fault(lambda: daemon.call('d.retrackers.enqueue_original', leased, daemon.call('d.local_id', leased),
                                    digest, '0'), 'reserved for a file operation')
    assert dispatch() == 'idle', 'pass launched while a stop lease refuses enqueue'
    jobs = []
    for index in range(9):
        hash_ = daemon.fixture('busy-' + str(index), start=False)
        marker = daemon.call('d.retrackers.enqueue_original', hash_, daemon.call('d.local_id', hash_), digest, '0')
        jobs.append((hash_, marker.rsplit(':', 1)[1], daemon.call('d.incarnation', hash_)))
    for hash_, tx, incarnation in jobs[:8]:
        assert daemon.call('system.retrackers.worker_start', '', hash_, tx, incarnation, user) == 'spawned'
    hash_, tx, incarnation = jobs[8]
    assert daemon.call('system.retrackers.worker_start', '', hash_, tx, incarnation, user) == 'full'
    assert dispatch() == 'idle', 'pass launched while every worker slot is busy'
    done = daemon.fixture('busy-done', start=False)
    marker = daemon.call('d.retrackers.enqueue_original', done, daemon.call('d.local_id', done), digest, '0')
    assert daemon.call('d.retrackers.finish_original', done, marker.rsplit(':', 1)[1],
                       daemon.call('d.incarnation', done)) == 'finalized'
    assert dispatch() == 'spawned', 'finalized claim was not dispatched with a full pool'


def dispatcher_gate_cost(daemon):
    # Waiting and terminal jobs without a completion marker are skipped before
    # any journal read: an unreadable claim of a waiting job does not count,
    # and two thousand waiting jobs keep the per-tick gate cheap.
    user = 'costuser'
    digest = hashlib.sha256(user.encode()).hexdigest()
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    daemon.call('method.set_key', '', 'rr.receipts.v1', 'pv:' + 'a' * 32, '1')
    daemon.call('method.set_key', '', 'rr.receipts.v1', 'pf:' + digest + ':' + 'b' * 64, '1')
    hashes = []
    for index in range(2000):
        info = {'length': 8, 'name': 'cost-' + str(index), 'piece length': 16384,
                'pieces': hashlib.sha1(b'xxxxxxxx').digest()}
        hashes.append(hashlib.sha1(encode(info)).hexdigest().upper())
        daemon.call('load.raw', '', xmlrpc.client.Binary(encode({'info': info})))
    daemon.wait(lambda: len(daemon.call('d.multicall2', '', 'main', 'd.hash=')) == len(hashes),
                'two thousand loads', timeout=120)
    due = int(time.time()) + 3600
    calls = []
    for index, hash_ in enumerate(hashes):
        queue = uuid.uuid4().hex
        retry = (f'r1:{queue}:1:{due}:transport-unavailable' if index % 2 else
                 f'r1:{queue}:6:0:capture-refused')
        calls += [{'methodName': 'd.custom.set', 'params': [hash_, 'retrackers-queued', 'q1:' + digest + ':' + queue]},
                  {'methodName': 'd.custom.set', 'params': [hash_, 'retrackers-retry', retry]}]
    for offset in range(0, len(calls), 400):
        daemon.call('system.multicall', calls[offset:offset + 400])
    unreadable = daemon.session / '.retrackers-migration' / (hashes[1] + '.preclaim.receipt')
    unreadable.write_text('preclaim:malformed\n')
    unreadable.chmod(0o600)
    assert daemon.call('system.retrackers.dispatcher_start', '', user) == 'idle'
    samples = []
    for _ in range(15):
        began = time.perf_counter()
        assert daemon.call('system.retrackers.dispatcher_start', '', user) == 'idle'
        samples.append(time.perf_counter() - began)
    median = sorted(samples)[len(samples) // 2]
    print(f'  dispatcher_start with 2000 waiting or terminal jobs: median {median * 1000:.1f} ms', flush=True)
    assert median < 0.02, f'gate costs {median * 1000:.1f} ms per tick'


def defer_storage_failure(daemon):
    # A storage failure before the durable release receipt leaves the exact
    # expected retry, status and claim, so the same CAS succeeds after repair.
    daemon.call('method.insert', '', 'rr.receipts.v1', 'multi|rlookup|static')
    hash_ = daemon.fixture('defer-storage-failure', start=False)
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    journal = daemon.session / '.retrackers-migration'
    prefix = hash_ + '.claim.' + tx
    receipt = journal / (prefix + '.late.receipt')
    sequence = int(receipt.read_text().split(':')[4]) + 1 if receipt.exists() else 1
    blocker = journal / (prefix + '.late.' + str(sequence) + '.a.torrent')
    blocker.mkdir(mode=0o700)
    prior = f'r1:{queue}:5:{int(time.time()) + 60}:capture-refused'
    daemon.call('d.custom.set', hash_, 'retrackers-retry', prior)
    terminal = f'r1:{queue}:6:0:capture-refused'
    defer = lambda: daemon.call('d.retrackers.defer_original', hash_, tx, incarnation, queue,
                                prior, terminal)
    expect_fault(defer, 'failed to rename torrent file')
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == prior, 'retry advanced without release'
    assert daemon.call('d.message', hash_) == '', 'exhaustion shown without release'
    assert daemon.call('d.custom', hash_, 'retrackers-recovery') == marker
    assert daemon.call('system.retrackers.original_claim', '', hash_).startswith('original:' + tx + ':')
    assert 'wp:' + local in daemon.call('method.list_keys', '', 'rr.receipts.v1')
    blocker.rmdir()
    assert defer() == 'cancelled', 'the same CAS must succeed after storage repair'
    assert daemon.call('d.custom', hash_, 'retrackers-retry') == terminal
    assert 'retries exhausted' in daemon.call('d.message', hash_)
    assert daemon.call('system.retrackers.original_claim', '', hash_) == 'none'
    assert daemon.call('d.custom', hash_, 'retrackers-queued').endswith(queue)


CAPSULE_SUFFIXES = ('.rollback.torrent', '.rollback.torrent.rtorrent',
                    '.rollback.torrent.libtorrent_resume', '.benc', '.backup.benc', '.target')


def stale_capsule_temp(daemon):
    # A killed publisher leaves unpublished ".<name>.tmp.<pid>[.<n>]" files.
    # A restarted daemon reuses the PID (PID 1 here): startup removes our own
    # temporaries and a later capture never collides with such a name.
    journal = daemon.session / '.retrackers-migration'
    hash_ = daemon.fixture('stale-capsule-temp')
    enqueue(daemon, hash_)
    pid = daemon.call('system.pid')

    def plant(directory, name, mode=0o600):
        path = directory / name
        path.write_bytes(b'interrupted publication')
        path.chmod(mode)
        return path

    stale = [plant(journal, '.' + hash_ + '.rollback.torrent.tmp.' + str(pid)),
             plant(journal, '.' + hash_ + '.benc.tmp.' + str(pid) + '.3'),
             plant(journal, '.' + hash_ + '.preclaim.receipt.tmp.77.12'),
             plant(daemon.session, '.' + hash_ + '.torrent.rtorrent.tmp.' + str(pid) + '.5')]
    unsafe = [plant(journal, '.' + hash_ + '.target.tmp.' + str(pid) + '.9', 0o644),
              journal / ('.' + hash_ + '.backup.benc.tmp.' + str(pid) + '.4'),
              journal / ('.' + hash_ + '.target.tmp.' + str(pid) + '.8'),
              plant(journal, '.' + hash_ + '.benc.tmp.' + str(pid) + '.6'),
              plant(journal, '.' + hash_ + '.benc.tmp.' + str(pid) + '.7')]
    unsafe[1].mkdir(mode=0o700)
    os.symlink('missing', unsafe[2])
    # A second link elsewhere, and another owner, are not our unpublished files.
    os.link(unsafe[3], daemon.directory / 'second-link')
    docker('exec', '--user', '0', daemon.name, 'chown', '1001:1001', '/data/.session/.retrackers-migration/' + unsafe[4].name)
    foreign = [plant(journal, '.' + hash_ + '.rollback.torrent.tmp.x'),
               plant(daemon.session, '.' + hash_ + '.torrent.tmp.' + str(pid)),
               plant(daemon.session, '.' + hash_ + '.other.tmp.1.1')]
    daemon.stop(kill=True)
    daemon.start()
    assert daemon.call('system.pid') == pid, 'this case needs the same PID after restart'
    survivors = [path.name for path in stale if os.path.lexists(path)]
    assert not survivors, 'unpublished temporaries survived restart: ' + repr(survivors)
    lost = [path.name for path in unsafe + foreign if not os.path.lexists(path)]
    assert not lost, 'startup removed entries it does not own: ' + repr(lost)
    log = (daemon.directory / 'native.log').read_text()
    assert 'RR migration startup kept 5 temporary-named entries' in log, log[-2000:]
    assert unsafe[3].stat().st_nlink == 2 and unsafe[4].stat().st_uid == 1001
    # A crash in this same process lifetime leaves names with the current PID.
    local, marker, tx, incarnation, queue = enqueue(daemon, hash_)
    names = ['.' + hash_ + suffix + '.tmp.' + str(pid) + ('.' + str(n) if n else '')
             for suffix in CAPSULE_SUFFIXES for n in range(0, 65)]
    collisions = [plant(journal, name) for name in names if not os.path.lexists(journal / name)]
    daemon.call('d.retrackers.capture_erase', hash_, local, marker, hash_, tx, '0' * 64)
    assert hash_ not in [r[0] for r in daemon.call('d.multicall2', '', 'main', 'd.hash=')]
    for suffix in CAPSULE_SUFFIXES:
        assert (journal / (hash_ + suffix)).stat().st_nlink == 1
    assert all(path.read_bytes() == b'interrupted publication' for path in collisions)


def shutdown_slow_tracker(daemon):
    # Shutdown must save before stopped announces can hang: a kill while the
    # tracker holds the first event=stopped request keeps every label edited
    # after the last explicit save.
    compile_fixture(daemon, 'stopped-tracker.cc', 'stopped-tracker')
    port = 45580
    docker('exec', '-d', daemon.name, '/data/stopped-tracker', str(port), '/data/tracker.ready',
           '/data/tracker-stopped')
    daemon.wait(lambda: (daemon.directory / 'tracker.ready').exists(), 'local tracker readiness')
    labels = {}
    for index in range(16):
        name = 'slow-stop-' + str(index)
        info = {'length': 8, 'name': name, 'piece length': 16384,
                'pieces': hashlib.sha1(b'xxxxxxxx').digest()}
        hash_ = hashlib.sha1(encode(info)).hexdigest().upper()
        (daemon.directory / 'dl' / name).write_bytes(b'xxxxxxxx')
        (daemon.directory / (name + '.torrent')).write_bytes(encode({
            'announce': f'http://127.0.0.1:{port}/announce', 'info': info}))
        daemon.call('load.start', '', '/data/' + name + '.torrent')
        labels[hash_] = 'latest-slow-label-' + str(index)
    daemon.wait(lambda: all(daemon.call('d.hash', h) == h for h in labels), 'tracker torrents loaded')
    daemon.wait(lambda: all((rows := daemon.call('t.multicall', h, '', 't.success_counter=', 't.is_busy='))
                            and all(row[0] > 0 and row[1] == 0 for row in rows) for h in labels),
                'initial announces accepted', timeout=60)
    daemon.call('session.save')
    daemon.wait(lambda: all((daemon.session / (h + '.torrent.rtorrent')).exists() for h in labels),
                'initial session triples')
    for hash_, label in labels.items():
        daemon.call('d.custom1.set', hash_, label)
    docker('kill', '--signal', 'SIGINT', daemon.name)
    daemon.wait(lambda: (daemon.directory / 'tracker-stopped').exists(), 'first held stopped announce')
    running = docker('inspect', '-f', '{{.State.Running}}', daemon.name).stdout.strip()
    docker('kill', daemon.name)
    assert running == 'true', 'daemon exited before its held stopped announce could be cut'
    daemon.wait(lambda: docker('inspect', '-f', '{{.State.Running}}', daemon.name).stdout.strip() == 'false',
                'killed daemon')
    assert docker('inspect', '-f', '{{.State.ExitCode}}', daemon.name).stdout.strip() == '137'
    docker('rm', daemon.name)
    daemon.start()
    lost = [h for h, label in labels.items() if daemon.call('d.custom1', h) != label]
    assert not lost, f'{len(lost)}/{len(labels)} labels lost while a stopped announce was held'
    print(f'  labels kept with a held stopped announce: {len(labels)}/{len(labels)}', flush=True)


def assert_saves_intact(daemon, hashes):
    errors = [line for line in (daemon.directory / 'native.log').read_text().splitlines()
              if 'Storage errors' in line]
    assert not errors, errors
    missing = [h + suffix for h in hashes for suffix in ('.torrent', '.torrent.rtorrent',
               '.torrent.libtorrent_resume') if not (daemon.session / (h + suffix)).exists()]
    assert not missing, 'torn session triples: ' + repr(missing)
    staging = sorted(x.name for x in daemon.session.iterdir() if x.name.endswith('.new'))
    assert not staging, 'overlapping saves left staging files: ' + repr(staging)


def session_save_overlap(daemon):
    # Two saves of one download share its fixed ".new" staging names. Full and
    # resume saves of a torrent with many files, requested back to back, must
    # run one after the other; overlapping writers unlinked each other's
    # staging files and could leave a torn triple.
    info = {'name': 'save-overlap', 'piece length': 1 << 20,
            'files': [{'length': 1, 'path': ['d' + str(index // 100), 'f' + str(index)]}
                      for index in range(2000)]}
    info['pieces'] = b'\0' * 20
    hash_ = hashlib.sha1(encode(info)).hexdigest().upper()
    daemon.call('load.raw', '', xmlrpc.client.Binary(encode({'info': info})))
    daemon.wait(lambda: daemon.call('d.hash', hash_) == hash_, 'large torrent load')
    time.sleep(2)
    for _ in range(20):
        daemon.call('system.multicall', [{'methodName': method, 'params': [hash_]}
                                         for method in ('d.save_full_session', 'd.save_resume') * 3])
    time.sleep(3)
    assert_saves_intact(daemon, [hash_])


def session_save_overlap_held(daemon):
    # The same rule with every save held between staging and renames, so a
    # mutation that lets saves of one path overlap fails deterministically.
    hashes = [daemon.fixture('save-overlap-' + str(index), start=False) for index in range(4)]
    daemon.call('session.save')
    daemon.call('system.multicall', [{'methodName': 'd.save_full_session', 'params': [hash_]}
                                     for hash_ in hashes])
    daemon.call('session.save')
    time.sleep(4)
    assert_saves_intact(daemon, hashes)


def shutdown_storage_errors(daemon):
    # Shutdown flushes share the session thread's storage-error policy: the
    # first error is logged with its text and later ones are counted, instead
    # of one error line per torrent and flush.
    hashes = [daemon.fixture('storage-error-' + str(index), start=False) for index in range(12)]
    daemon.call('session.save')
    daemon.wait(lambda: all((daemon.session / (h + '.torrent.rtorrent')).exists() for h in hashes),
                'initial session triples')
    time.sleep(1)
    for hash_ in hashes:
        daemon.call('d.custom1.set', hash_, 'unsaved')
    daemon.session.chmod(0o2500)
    try:
        # The session thread fails these first: one line, eleven suppressed.
        daemon.call('session.save')
        time.sleep(2)
        daemon.stop()
    finally:
        daemon.session.chmod(0o2700)
    lines = [line for line in (daemon.directory / 'native.log').read_text().splitlines()
             if 'Storage errors' in line]
    assert lines and 'failed to' in lines[0], lines
    summaries = [re.search(r'flushing session data: ignored:(\d+), earlier ignored:(\d+)', line)
                 for line in lines if 'flushing session data' in line]
    assert summaries and all(summaries), lines
    assert all(int(match.group(1)) <= len(hashes) for match in summaries), lines
    assert int(summaries[0].group(2)) == len(hashes) - 1, lines
    assert len(lines) <= 3, f'{len(lines)} storage error lines for {len(hashes)} torrents'
    print(f'  storage error lines for {len(hashes)} torrents: {len(lines)}', flush=True)


CASES = {'stop-cat': stop_cat, 'claim-restart': claim_restart,
         'claim-finish': claim_finish, 'owner-finalize': owner_finalize,
         'metadata-start': metadata_start, 'metadata-restart': metadata_restart,
         'stale-counter': stale_counter,
         'unsafe-async-path': unsafe_async_path, 'erase-guard-preflight': erase_guard_preflight}
CASES.update({'rename-publication': rename_fallback, 'rename-einval': rename_fallback,
              'rename-enosys': rename_fallback, 'rename-eopnotsupp': rename_fallback,
              'uncaptured-remove': uncaptured_remove,
              'retry-cancel': retry_cancel, 'retry-wait': retry_wait,
              'retry-new-queue': retry_new_queue,
              'captured-remove': captured_remove,
              'partial-capture-remove': partial_capture_remove,
              'capture-existing-capsule': capture_existing_capsule, 'defer-crash': defer_crash,
              'defer-storage-failure': defer_storage_failure,
              'defer-late-failure': defer_late_failure,
              'defer-late-failure-cut': defer_late_failure_cut,
              'erase-storage-refusal': erase_storage_refusal,
              'enqueue-storage-refusal': enqueue_storage_refusal,
              'enqueue-storage-full': enqueue_storage_full,
              'late-save-schedule': late_save_schedule,
              'worker-death-erased': worker_death_erased,
              'worker-death-candidate': worker_death_candidate,
              'worker-death-committed': worker_death_committed,
              'worker-death-restart': worker_death_restart,
              'worker-death-reprocessed': worker_death_reprocessed,
              'worker-death-partial': worker_death_partial,
              'worker-death-loop': worker_death_loop,
              'capture-crash-intent': lambda daemon: capture_phase(daemon, 'intent', stop=False),
              'capture-crash-quiesced': lambda daemon: capture_phase(daemon, 'quiesced', stop=False),
              'capture-crash-erased': lambda daemon: capture_phase(daemon, 'erased', stop=False),
              'capture-crash-committed': lambda daemon: capture_phase(daemon, 'committed', stop=False),
              'capture-stop-erased': lambda daemon: capture_phase(daemon, 'erased', stop=True),
              'capture-stop-candidate': lambda daemon: capture_phase(daemon, 'candidate', stop=True),
              'capture-stop-committed': lambda daemon: capture_phase(daemon, 'committed', stop=True),
              'retry-cleared-dequeue': retry_cleared_dequeue,
              'pause-kept': pause_kept, 'samehash-resume': samehash_resume,
              'immediate-shutdown': immediate_shutdown,
              'move-no-adopter': move_no_adopter,
              'move-sync-failure': move_sync_failure,
              'move-publish-before': move_publication_crash,
              'move-publish-after': lambda daemon: move_publication_crash(daemon, after=True),
              'move-finish-crash': lambda daemon: move_publication_crash(daemon, finish=True),
              'move-claim-crash': move_claim_crash,
              'move-supersede-crash': lambda daemon: move_claim_crash(daemon, supersede=True), 'move-queue': move_queue, 'move-supersede': move_supersede, 'move-corrupt': move_corrupt, 'move-foreign-leaf': move_foreign_leaf,
              'move-pool': move_pool, 'move-worker-exhaustion': move_worker_exhaustion, 'move-capture-restart': move_capture_restart, 'move-storage': move_storage,
              'samehash-prepared-prior': samehash_prepared_prior,
              'samehash-prepared-foreign': lambda daemon: samehash_prepared_prior(daemon, foreign=True),
              'erase-retire-batch': erase_retire_batch,
              'dispatcher-gate-busy': dispatcher_gate_busy,
              'dispatcher-gate-cost': dispatcher_gate_cost,
              'dispatcher-gate': dispatcher_gate,
              'stale-capsule-temp': stale_capsule_temp,
              'shutdown-slow-tracker': shutdown_slow_tracker,
              'shutdown-storage-errors': shutdown_storage_errors,
              'session-save-overlap': session_save_overlap,
              'session-save-overlap-held': session_save_overlap_held,
              'startup-hold-visible': startup_hold_visible,
              'shutdown-early-save': shutdown_early_save,
              'shutdown-hook-save': lambda daemon: shutdown_early_save(daemon, hook=True)})
CASES.update({'owner-cut-before': owner_cut, 'owner-cut-after': owner_cut,
              'owner-cut-retry': owner_cut_retry, 'owner-tombstone-cut': owner_tombstone_cut,
              'owner-tombstone-startup': owner_tombstone_startup})
assert set(CUTS) <= set(CASES), set(CUTS) - set(CASES)


if __name__ == '__main__':
    arguments, cuts = [], False
    for argument in sys.argv[1:]:
        if argument == '--cuts':
            cuts = True
        elif argument == '--ninep':
            SETTINGS['seccomp'] = 'ninep'
        elif argument.startswith('--prefix='):
            SETTINGS['prefix'] = argument.split('=', 1)[1]
        elif argument.startswith('--label='):
            SETTINGS['labels'].append(argument.split('=', 1)[1])
        elif argument.startswith('--'):
            raise SystemExit('unknown option ' + argument + '\n' + __doc__)
        else:
            arguments.append(argument)
    if len(arguments) < 3:
        raise SystemExit(__doc__)
    image, tree, scratch = arguments[:3]
    unknown = [case for case in arguments[3:] if case not in CASES]
    if unknown:
        raise SystemExit('unknown case ' + ', '.join(unknown))
    if SETTINGS['seccomp']:
        profile = pathlib.Path(scratch).resolve() / 'ninep-seccomp.json'
        profile.parent.mkdir(parents=True, exist_ok=True)
        profile.write_text(json.dumps(NINEP_SECCOMP))
        SETTINGS['seccomp'] = str(profile)
    selected = arguments[3:] or [case for case in CASES if cuts or case not in CUTS]
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

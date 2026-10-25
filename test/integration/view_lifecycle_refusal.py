#!/usr/bin/env python3
"""Check view callback refusal preserves existing native recovery ownership.

Usage: view_lifecycle_refusal.py BUILDER_IMAGE BUILD_TREE SCRATCH [CASE ...]
Uses native_regressions' isolated daemon and fixture owners; no tracker network.
"""
import pathlib
import sys
import traceback

sys.dont_write_bytecode = True
import native_regressions as native


GETTERS = ('d.state', 'd.is_open', 'd.is_active', 'd.stop_generation', 'd.message', 'd.directory')


def invoke_filter(daemon, hash_, command):
    daemon.call('view.add', '', 'refusal')
    daemon.call('method.insert', '', 'refusal.command', 'simple',
                'd.custom.set=refusal-entered,yes;' + command + ';d.custom.set=refusal-after,yes;cat=1')
    daemon.call('view.filter', '', 'refusal',
                'branch="equal=d.hash=,cat=' + hash_ + '","catch=refusal.command=",false=')
    assert daemon.call('d.custom', hash_, 'refusal-entered') == 'yes'
    assert daemon.call('d.custom', hash_, 'refusal-after') == ''
    assert "Caught exception: 'View filter and sort commands cannot change views or downloads.'." in (
        daemon.directory / 'native.log').read_text()


def verify_case(daemon, case):
    pid = daemon.call('system.pid')
    payload = daemon.directory / 'dl' / 'target'
    if case == 'directory':
        payload.write_bytes(b'xxxxxxxx')
    hash_ = daemon.fixture('target', start=True)
    witness = daemon.fixture('unrelated', start=False)
    daemon.wait(lambda: daemon.call('d.is_active', hash_) == 1, 'active target')
    tied = daemon.directory / 'target.torrent'
    original_tied = tied.read_bytes()
    job = 'a' * 32
    if case == 'directory':
        daemon.wait(lambda: daemon.call('d.complete', hash_) == 1, 'completed target')
        payload.unlink()
        command = 'd.directory.set=/data/dl/relocated'
    elif case.startswith('erasedata-'):
        values = native.erasedata_tuple(daemon, hash_)
        assert native.erasedata_call(daemon, 'prepare', values) == 'prepared'
        prior_record = native.erasedata_call(daemon, 'state', values)
        operation = case.removeprefix('erasedata-')
        command = 'system.erasedata.' + operation + '=' + ','.join(values)
    else:
        receipt = daemon.call('d.stop_close_claim', hash_, job)
        token = receipt.split('|', 1)[0]
        if case == 'claim-ack':
            assert daemon.call('d.release_stop_close_claim', hash_, token) == 1
        prior_record = daemon.call('d.stop_close_claim_state', hash_, job)
        command = {'claim-retry': 'd.stop_close_claim=' + job,
                   'claim-start': 'd.start_if_stop_close_claim=' + token,
                   'claim-release': 'd.release_stop_close_claim=' + token,
                   'claim-ack': 'd.ack_stop_close_claim=' + job}[case]
    before = [daemon.call(method, hash_) for method in GETTERS]
    invoke_filter(daemon, hash_, command)
    assert [daemon.call(method, hash_) for method in GETTERS] == before, case
    assert tied.read_bytes() == original_tied, case
    assert daemon.call('system.pid') == pid
    assert daemon.call('d.hash', witness) == witness
    if case == 'directory':
        payload.write_bytes(b'xxxxxxxx')
        daemon.call('view.filter', '', 'refusal', 'cat=1')
        daemon.call('d.directory.set', hash_, '/data/dl/relocated')
        assert daemon.call('d.directory', hash_) == '/data/dl/relocated'
    elif case.startswith('erasedata-'):
        assert native.erasedata_call(daemon, 'state', values) == prior_record, case
        daemon.call('view.filter', '', 'refusal', 'cat=1')
        assert native.erasedata_call(daemon, 'cancel', values) == 'cancelled'
        assert native.erasedata_call(daemon, 'state', values)['phase'] == 'missing'
    else:
        assert daemon.call('d.stop_close_claim_state', hash_, job) == prior_record, case
        daemon.call('view.filter', '', 'refusal', 'cat=1')
        assert daemon.call('d.release_stop_close_claim', hash_, token) == 1
        assert daemon.call('d.ack_stop_close_claim', hash_, job) == 1
        assert daemon.call('d.stop_close_claim_state', hash_, job) == 'absent'
    assert daemon.call('d.hash', hash_) == hash_


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    image, tree, scratch = sys.argv[1:4]
    pathlib.Path(scratch).mkdir(parents=True, exist_ok=True)
    native.SETTINGS['prefix'] = 'view-lifecycle-'
    failed = 0
    for case in sys.argv[4:] or ('erasedata-cancel', 'erasedata-erase', 'claim-retry', 'claim-start', 'claim-release', 'claim-ack', 'directory'):
        daemon = native.Daemon(image, tree, scratch, case)
        try:
            daemon.start()
            verify_case(daemon, case)
            daemon.stop()
            print('PASS - ' + case, flush=True)
        except Exception:
            failed += 1
            traceback.print_exc()
            print('FAIL - ' + case, flush=True)
        finally:
            native.docker('rm', '-f', daemon.name, check=False)
    return int(failed != 0)


if __name__ == '__main__':
    raise SystemExit(main())

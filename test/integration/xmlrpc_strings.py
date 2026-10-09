#!/usr/bin/env python3
"""Check XML-RPC string byte preservation with strict Expat decoding.

Usage: xmlrpc_strings.py BUILDER_IMAGE BUILD_TREE SCRATCH
Uses the native regression harness's isolated daemon and cleanup. The tree must
contain the exact daemon to test; captures stay in the caller's scratch tree.
"""
import hashlib
import sys
import xmlrpc.client

sys.dont_write_bytecode = True
from native_regressions import Daemon, docker, encode


def raw_bytes(value):
    return value.data if isinstance(value, xmlrpc.client.Binary) else value.encode('utf-8')


def check_strings(daemon):
    # Captured FIN-HTTP C1 metainfo name (2026-10-08): the original daemon
    # emitted SOH literally in d.multicall2 and Expat rejected the response.
    name = b'ctl\x01name.bin'
    info = {'length': 8, 'name': name, 'piece length': 16384,
            'pieces': hashlib.sha1(b'xxxxxxxx').digest()}
    hash_ = hashlib.sha1(encode(info)).hexdigest().upper()
    daemon.call('load.raw', '', xmlrpc.client.Binary(encode({'info': info})))
    daemon.wait(lambda: daemon.call('d.hash', hash_) == hash_, 'control-byte torrent load')
    assert raw_bytes(daemon.call('d.multicall2', '', 'main', 'd.name=')[0][0]) == name

    normal = [b'', b'normal', b'<&>"\'', b'tab\tLF\nend', b'DEL\x7f',
              'чао😊\u0080\ud7ff\ue000\ufffd\U0010ffff'.encode()]
    binary = [b'before' + bytes([ch]) + b'after' for ch in range(32) if ch not in (9, 10)]
    binary += [b'\xc3(', b'\x80', b'\xe2\x82', b'\xc0\x80', b'\xc1\xbf',
               b'\xe0\x80\x80', b'\xed\xa0\x80', b'\xf0\x80\x80\x80',
               b'\xf4\x90\x80\x80', b'\xf5\x80\x80\x80', b'\xef\xbf\xbe', b'\xef\xbf\xbf',
               b'\r\n', b'\r', b'\x00tail', b'head\x00tail']
    for expected_binary, samples in ((False, normal), (True, binary)):
        for raw in samples:
            value = daemon.call('cat', '', xmlrpc.client.Binary(raw))
            assert isinstance(value, xmlrpc.client.Binary) == expected_binary, (raw, type(value))
            assert raw_bytes(value) == raw, (raw, value)
    # faultString is required by XML-RPC to remain a string, including in
    # per-call faults. Escapes preserve diagnostic bytes exposed by what();
    # that C-string API cannot expose any suffix after an embedded NUL.
    unsafe_view = xmlrpc.client.Binary(b'missing\x01view\xc3(\\tail')
    try:
        daemon.call('view.size', '', unsafe_view)
    except xmlrpc.client.Fault as fault:
        text = fault.faultString
        assert isinstance(text, str), type(text)
        assert text.startswith('escaped bytes: ') and '\\x01' in text and '\\xC3' in text and '\\x5C' in text, text
    else:
        raise AssertionError('missing unsafe view did not fault')
    fault = daemon.call('system.multicall', [{'methodName': 'view.size', 'params': ['', unsafe_view]}])[0]
    assert isinstance(fault['faultString'], str), fault
    assert fault['faultString'] == text, (fault, text)
    try:
        daemon.call('view.size', '', 'missing-safe-view')
    except xmlrpc.client.Fault as fault:
        assert 'missing-safe-view' in fault.faultString and not fault.faultString.startswith('escaped bytes: ')
    else:
        raise AssertionError('missing safe view did not fault')
    # User-owned custom keys can be arbitrary bytes. XML-RPC has no binary
    # member-name type, so refuse unsafe maps without changing stored keys.
    daemon.call('d.custom.set', hash_, 'partial-safe-key', xmlrpc.client.Binary(b'value\x01'))
    assert raw_bytes(daemon.call('d.custom.items', hash_)['partial-safe-key']) == b'value\x01'
    for key in (b'bad\x01key', b'bad\rkey', b'bad\xffkey', b'bad\x00key'):
        daemon.call('d.custom.set', hash_, xmlrpc.client.Binary(key), 'stored-value')
        assert daemon.call('d.custom', hash_, xmlrpc.client.Binary(key)) == 'stored-value'
        assert key in [raw_bytes(k) for k in daemon.call('d.custom.keys', hash_)]
        for method, args in (
                ('d.custom.items', [hash_]),
                ('system.multicall', [[{'methodName': 'd.custom.items', 'params': [hash_]}]])):
            try:
                daemon.call(method, *args)
            except xmlrpc.client.Fault as fault:
                assert fault.faultCode == -501 and fault.faultString == 'struct member name is not XML 1.0 text', fault
            else:
                raise AssertionError('unsafe member name did not refuse ' + method)
    print('strict XML-RPC name + %d string roundtrips + standard direct/multicall faults + unsafe map-key refusals passed' %
          (len(normal) + len(binary)))


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    daemon = Daemon(*sys.argv[1:], 'xmlrpc-strings')
    try:
        daemon.start()
        check_strings(daemon)
        daemon.stop()
    finally:
        # start can fail after creating the container; stop can assert before
        # its own removal. Keep those failures while always retiring our actor.
        docker('rm', '-f', daemon.name, check=False)


if __name__ == '__main__':
    main()

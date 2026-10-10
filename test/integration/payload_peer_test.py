#!/usr/bin/env python3
"""Check a compiled payload-peer fixture using synthetic loopback traffic only.

Usage: payload_peer_test.py /path/to/payload-peer [-v]
The binary is compiled separately, including in the matching image builder when
the production runtime has no compiler. No daemon or Docker resources are used.
"""
import hashlib
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest


PIECE = 16384
PAYLOAD = bytes(range(256)) * 128 + b'last-partial-piece' * 15
HASH = hashlib.sha1(b'synthetic payload-peer protocol selftest').digest()
BINARY = None


def receive(connection, count):
    result = bytearray()
    while len(result) < count:
        part = connection.recv(count - len(result))
        if not part:
            raise AssertionError('fixture disconnected before the expected response')
        result.extend(part)
    return bytes(result)


def frame(connection, body):
    connection.sendall(struct.pack('!I', len(body)) + body)


def response(connection):
    length = struct.unpack('!I', receive(connection, 4))[0]
    if length > 65536:
        raise AssertionError('fixture response oversized')
    return receive(connection, length)


def request(piece, begin, count):
    return b'\x06' + struct.pack('!III', piece, begin, count)


class Peer:
    def __init__(self, timeout=5):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reserve:
            reserve.bind(('127.0.0.1', 0))
            self.port = reserve.getsockname()[1]
        self.temporary = tempfile.TemporaryDirectory(prefix='rr-payload-peer-')
        self.directory = pathlib.Path(self.temporary.name)
        self.payload = self.directory / 'payload'
        self.payload.write_bytes(PAYLOAD)
        self.prefix = self.directory / 'gate'
        self.process = subprocess.Popen(
            [str(BINARY), str(self.payload), HASH.hex(), str(PIECE), str(self.port),
             str(self.prefix), str(timeout)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.connection = None

    def marker(self, suffix):
        return pathlib.Path(str(self.prefix) + suffix)

    def wait(self, suffix, timeout=4):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.marker(suffix).exists():
                return self.marker(suffix).read_text()
            if self.process.poll() is not None:
                raise AssertionError('fixture exited before ' + suffix + ': ' +
                                     self.process.stderr.read().decode())
            time.sleep(.01)
        raise AssertionError('fixture marker deadline: ' + suffix)

    def connect(self, hash_=HASH):
        self.wait('.ready')
        self.connection = socket.create_connection(('127.0.0.1', self.port), timeout=2)
        handshake = b'\x13BitTorrent protocol' + bytes(8) + hash_ + b'-CLIENT-000000000000'
        assert len(handshake) == 68
        # Exercise exact reads across a fragmented handshake.
        self.connection.sendall(handshake[:7])
        self.connection.sendall(handshake[7:])
        if hash_ != HASH:
            return
        answer = receive(self.connection, 68)
        assert answer[:20] == b'\x13BitTorrent protocol' and answer[28:48] == HASH
        assert response(self.connection) == b'\x05\xe0'
        assert response(self.connection) == b'\x01'
        self.wait('.connected')

    def release(self, value=b'release\n'):
        temporary = self.marker('.release-tmp')
        temporary.write_bytes(value)
        os.replace(temporary, self.marker('.release'))

    def failed(self, reason):
        self.process.wait(timeout=3)
        assert self.process.returncode == 1, self.process.returncode
        assert reason in self.marker('.error').read_text()
        assert not self.marker('.done').exists()

    def close(self):
        if self.connection:
            self.connection.close()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        self.process.communicate(timeout=1)
        self.temporary.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class PayloadPeerTest(unittest.TestCase):
    def test_release_gates_every_payload_byte_and_completes_exact_partial_tail(self):
        with Peer() as peer:
            peer.connect()
            frame(peer.connection, b'\x02')
            frame(peer.connection, request(0, 0, PIECE))
            self.assertIn('piece=0 begin=0 length=16384', peer.wait('.requested'))
            peer.connection.settimeout(.15)
            with self.assertRaises(socket.timeout):
                peer.connection.recv(1)
            self.assertFalse(peer.marker('.done').exists())
            peer.release()
            peer.connection.settimeout(2)
            self.assertEqual(response(peer.connection), b'\x07' + struct.pack('!II', 0, 0) + PAYLOAD[:PIECE])
            # A duplicated first block cannot make the coverage marker complete.
            frame(peer.connection, request(0, 0, PIECE))
            self.assertEqual(response(peer.connection), b'\x07' + struct.pack('!II', 0, 0) + PAYLOAD[:PIECE])
            self.assertFalse(peer.marker('.done').exists())
            for piece in (1, 2):
                data = PAYLOAD[piece * PIECE:(piece + 1) * PIECE]
                frame(peer.connection, request(piece, 0, len(data)))
                self.assertEqual(response(peer.connection), b'\x07' + struct.pack('!II', piece, 0) + data)
            peer.connection.shutdown(socket.SHUT_WR)
            self.assertEqual(peer.connection.recv(1), b'')
            peer.process.wait(timeout=2)
            self.assertEqual(peer.process.returncode, 0)
            self.assertEqual(peer.marker('.done').read_text(), 'payload-bytes=' + str(len(PAYLOAD)) + '\n')
            self.assertFalse(peer.marker('.error').exists())

    def test_pipelined_requests_and_unread_tail_finish_with_full_payload_and_clean_eof(self):
        with Peer() as peer:
            peer.connect()
            packets = []
            expected = []
            for piece in range((len(PAYLOAD) + PIECE - 1) // PIECE):
                data = PAYLOAD[piece * PIECE:(piece + 1) * PIECE]
                body = request(piece, 0, len(data))
                packets.append(struct.pack('!I', len(body)) + body)
                expected.append(b'\x07' + struct.pack('!II', piece, 0) + data)
            # These frames remain unread when the last request completes coverage.
            packets.append(bytes(4) * 64)
            peer.connection.sendall(b''.join(packets))
            peer.wait('.requested')
            peer.release()
            peer.connection.shutdown(socket.SHUT_WR)
            time.sleep(.05)
            for body in expected:
                self.assertEqual(response(peer.connection), body)
            self.assertEqual(peer.connection.recv(1), b'')
            peer.process.wait(timeout=2)
            self.assertEqual(peer.process.returncode, 0)
            self.assertEqual(peer.marker('.done').read_text(), 'payload-bytes=' + str(len(PAYLOAD)) + '\n')
            self.assertFalse(peer.marker('.error').exists())

    def test_post_payload_drain_uses_the_existing_global_deadline(self):
        with Peer(timeout=1) as peer:
            peer.connect()
            for piece in range((len(PAYLOAD) + PIECE - 1) // PIECE):
                data = PAYLOAD[piece * PIECE:(piece + 1) * PIECE]
                frame(peer.connection, request(piece, 0, len(data)))
                if piece == 0:
                    peer.wait('.requested')
                    peer.release()
                self.assertEqual(response(peer.connection), b'\x07' + struct.pack('!II', piece, 0) + data)
            # Keep our write half open after the fixture's FIN; draining is bounded.
            self.assertEqual(peer.connection.recv(1), b'')
            peer.process.wait(timeout=3)
            self.assertEqual(peer.process.returncode, 1)
            self.assertIn('deadline expired', peer.marker('.error').read_text())
            # Send coverage alone does not prove a graceful transport close.
            self.assertEqual(peer.marker('.done').read_text(), 'payload-bytes=' + str(len(PAYLOAD)) + '\n')

    def test_wrong_info_hash_is_refused_before_payload_or_request_marker(self):
        with Peer() as peer:
            peer.connect(bytes(20))
            peer.failed('handshake mismatch')
            self.assertFalse(peer.marker('.requested').exists())

    def test_request_bounds_and_canonical_thirteen_byte_body(self):
        invalid = ((request(3, 0, 1), 'bounds'),
                   (request(0, PIECE, 1), 'bounds'),
                   (request(0, 0, 0), 'bounds'),
                   (request(0, 0, PIECE + 1), 'bounds'),
                   (request(0, PIECE - 1, 2), 'bounds'),
                   (request(2, len(PAYLOAD) - 2 * PIECE - 1, 2), 'bounds'),
                   (request(0, 0, 1)[:-1], 'length'),
                   (request(0, 0, 1) + b'x', 'length'))
        for body, reason in invalid:
            with self.subTest(body=body), Peer() as peer:
                peer.connect()
                frame(peer.connection, body)
                peer.failed('request ' + reason + ' invalid')
                self.assertFalse(peer.marker('.requested').exists())

    def test_oversized_frame_is_refused_before_body_read(self):
        with Peer() as peer:
            peer.connect()
            peer.connection.sendall(struct.pack('!I', 65537))
            peer.failed('message oversized')

    def test_invalid_or_nonregular_release_cannot_open_gate(self):
        for mode in ('bytes', 'symlink', 'fifo'):
            with self.subTest(mode=mode), Peer() as peer:
                peer.connect()
                frame(peer.connection, request(0, 0, PIECE))
                peer.wait('.requested')
                if mode == 'bytes':
                    peer.release(b'releaseX')
                elif mode == 'symlink':
                    peer.marker('.release').symlink_to(peer.payload)
                else:
                    os.mkfifo(peer.marker('.release'))
                peer.failed('release marker')

    def test_missing_connection_and_missing_release_have_bounded_deadlines(self):
        with Peer(timeout=1) as peer:
            peer.wait('.ready')
            peer.failed('deadline expired')
        with Peer(timeout=1) as peer:
            peer.connect()
            frame(peer.connection, request(0, 0, PIECE))
            peer.wait('.requested')
            peer.failed('deadline expired')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    BINARY = pathlib.Path(sys.argv.pop(1)).resolve(strict=True)
    unittest.main()

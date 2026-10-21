"""Verify incoming/outgoing sessions, framing, and queued PING/PONG responses."""

import argparse
import os
from pathlib import Path
import re
import selectors
import socket
import struct
import subprocess
import sys
import time

PING, PONG, HELLO = 0, 1, 2
HEADER = struct.Struct('!BQI')  # type, request ID, payload length; big-endian


def frame(kind, request_id, payload=b''):
    return HEADER.pack(kind, request_id, len(payload)) + payload


def read_exact(connection, count):
    data = bytearray()
    while len(data) < count:
        part = connection.recv(count - len(data))
        if not part:
            raise AssertionError('Connection closed before a complete message arrived')
        data.extend(part)
    return bytes(data)


def read_message(connection):
    kind, request_id, size = HEADER.unpack(read_exact(connection, HEADER.size))
    return kind, request_id, read_exact(connection, size)


def handshake(connection, peer_id):
    kind, _, payload = read_message(connection)
    assert kind == HELLO and payload, (kind, payload)
    connection.sendall(frame(HELLO, 1, peer_id))


class RelayProcess:
    def __init__(self, executable, *args):
        self.process = subprocess.Popen(
            [str(executable), *args], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self.output = ''
        self.selector = selectors.DefaultSelector()
        for pipe in (self.process.stdout, self.process.stderr):
            self.selector.register(pipe, selectors.EVENT_READ)

    def wait_for(self, text):
        deadline = time.monotonic() + 5
        while text not in self.output:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f'Timed out waiting for {text!r}:\n{self.output}')
            for key, _ in self.selector.select(remaining):
                data = os.read(key.fileobj.fileno(), 4096)
                if not data:
                    self.selector.unregister(key.fileobj)
                self.output += data.decode(errors='replace')
            if self.process.poll() is not None and text not in self.output:
                raise AssertionError(f'Relay exited early:\n{self.output}')

    def stop(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.write(b'\n')
                self.process.stdin.flush()
            self.process.wait(timeout=5)
            if self.process.returncode != 0:
                raise AssertionError(f'Relay exited with {self.process.returncode}:\n{self.output}')
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.selector.close()
            for pipe in (self.process.stdin, self.process.stdout, self.process.stderr):
                pipe.close()


def test_incoming(executable):
    relay = RelayProcess(executable, '--listen', '0')
    try:
        relay.wait_for('Press Enter to stop.')
        port = int(re.search(r'Listening on port (\d+)', relay.output).group(1))
        with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
            handshake(client, b'first-peer')
            first = frame(PING, 0x0102030405060708, b'fragmented')
            client.sendall(first[:3])
            time.sleep(0.05)
            client.sendall(first[3:])
            assert read_message(client) == (PONG, 0x0102030405060708, b'fragmented')

            # A burst makes multiple replies queue while one write is active.
            payloads = [b'', bytes(range(256)), b'x' * 65536] * 10
            client.sendall(b''.join(frame(PING, 100 + i, p) for i, p in enumerate(payloads)))
            for i, payload in enumerate(payloads):
                assert read_message(client) == (PONG, 100 + i, payload)
            client.sendall(frame(PONG, 500))
            relay.wait_for('Received PONG, request 500')
            client.settimeout(0.15)
            try:
                unexpected = client.recv(1)
            except socket.timeout:
                pass  # PONG must not trigger a response loop.
            else:
                raise AssertionError(f'Unexpected response or closure after PONG: {unexpected!r}')
            # Both sessions stay usable while the manager owns them.
            client.settimeout(5)
            with socket.create_connection(('127.0.0.1', port), timeout=5) as second:
                handshake(second, b'second-peer')
                client.sendall(frame(PING, 600, b'first'))
                second.sendall(frame(PING, 601, b'second'))
                assert read_message(client) == (PONG, 600, b'first')
                assert read_message(second) == (PONG, 601, b'second')
        relay.wait_for('Connection 1 closed')

        # Malformed frames close only their own session.
        for malformed in (HEADER.pack(99, 0, 0), HEADER.pack(PING, 0, 1048577)):
            with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
                handshake(client, b'malformed-peer')
                client.sendall(malformed)
                assert client.recv(1) == b''
        # The listener and registry still work after disconnected sessions.
        with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
            handshake(client, b'reconnected-peer')
            client.sendall(frame(PING, 999, b'still listening'))
            assert read_message(client) == (PONG, 999, b'still listening')
        print('PASS incoming: fragmented header, burst queue, binary/empty payloads, IDs, PONG, disconnects, invalid frames')
    finally:
        relay.stop()


def test_outgoing(executable):
    with socket.socket() as server:
        server.bind(('127.0.0.1', 0))
        server.listen()
        server.settimeout(5)
        port = server.getsockname()[1]
        relay = RelayProcess(executable, '--connect', '127.0.0.1', str(port))
        try:
            client, _ = server.accept()
            with client:
                client.settimeout(5)
                handshake(client, b'outgoing-peer')
                client.sendall(frame(PONG, 1))
                relay.wait_for('Received PONG, request 1')
                # Outgoing connections use the exact same receive/respond path.
                client.sendall(frame(PING, 77, b'outgoing session'))
                assert read_message(client) == (PONG, 77, b'outgoing session')
            relay.wait_for('Connection 1 closed')
            print('PASS outgoing: Hello handshake registers session, receives PONG, and answers PING')
        finally:
            relay.stop()


def test_registration(executable):
    relay = RelayProcess(executable, '--listen', '0')
    sockets = []
    try:
        relay.wait_for('Press Enter to stop.')
        port = int(re.search(r'Listening on port (\d+)', relay.output).group(1))
        def connect():
            client = socket.create_connection(('127.0.0.1', port), timeout=5)
            sockets.append(client)
            kind, _, payload = read_message(client)
            assert kind == HELLO and payload
            return client

        first = connect()
        first.sendall(frame(HELLO, 1, b'same-peer') + frame(PING, 2))
        assert read_message(first) == (PONG, 2, b'')
        # Repeated Hello must neither reply nor register again.
        first.sendall(frame(HELLO, 3, b'same-peer') + frame(PING, 4))
        assert read_message(first) == (PONG, 4, b'')
        duplicate = connect()
        duplicate.sendall(frame(HELLO, 1, b'same-peer'))
        assert duplicate.recv(1) == b''
        first.sendall(frame(PING, 5))
        assert read_message(first) == (PONG, 5, b'')
        first.close()
        relay.wait_for('Connection 1 closed')
        replacement = connect()
        replacement.sendall(frame(HELLO, 1, b'same-peer') + frame(PING, 6))
        assert read_message(replacement) == (PONG, 6, b'')
        for kind, payload in ((HELLO, b''), (PING, b'')):
            invalid = connect()
            invalid.sendall(frame(kind, 1, payload))
            assert invalid.recv(1) == b''
        changed = connect()
        changed.sendall(frame(HELLO, 1, b'original') + frame(HELLO, 2, b'changed'))
        assert changed.recv(1) == b''
        # Shutdown must close both registered and unintroduced sessions.
        pending = connect()
        relay.stop()
        assert pending.recv(1) == b''
        assert replacement.recv(1) == b''
        print('PASS registration: duplicate/repeated/changed IDs, reconnect, invalid greeting, pending shutdown')
    finally:
        for client in sockets:
            client.close()
        if relay.process.poll() is None:
            relay.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--relay', type=Path,
                        default=Path(__file__).resolve().parent / 'cmake-build-debug' / 'Relay')
    args = parser.parse_args()
    try:
        test_incoming(args.relay.resolve())
        test_outgoing(args.relay.resolve())
        test_registration(args.relay.resolve())
    except (OSError, AssertionError, subprocess.TimeoutExpired) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1
    print('All tests passed. Test servers used automatically allocated ports.')
    return 0


if __name__ == '__main__':
    sys.exit(main())

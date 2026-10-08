"""Start Relay and verify reception of different framed messages on one socket."""

import argparse
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import sys
import time


# Matches the current MessageType enum in ConnectionSession.h.
MESSAGE_TYPES = [
    "Ping", "Pong", "Authentication", "FileListRequest", "FileListResponse",
    "FileRequest", "FileChunk", "TransferComplete", "Error",
]


def run_test(executable):
    process = subprocess.Popen(
        [str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
    )
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(timeout=5):
                raise RuntimeError("Relay did not start listening within 5 seconds")
            first_line = process.stdout.readline()
        if "Successfully listening" not in first_line:
            raise RuntimeError("Relay failed to start: " + process.stderr.read())

        payloads = [
            b"", b"pong", b"test-peer", b"", b"[]", b"photos/example.png",
            bytes(range(256)), b"", b"test error",
        ]
        frames = [
            struct.pack("!BQI", message_type, 100 + message_type, len(payload)) + payload
            for message_type, payload in enumerate(payloads)
        ]

        with socket.create_connection(("127.0.0.1", 12345), timeout=5) as client:
            # Exercise a header arriving across multiple network reads.
            client.sendall(frames[0][:3])
            time.sleep(0.05)
            client.sendall(frames[0][3:])
            # Exercise consecutive frames without waiting between messages.
            client.sendall(b"".join(frames[1:]))
            client.shutdown(socket.SHUT_WR)
            # Relay closes after processing the messages and reading EOF.
            while client.recv(4096):
                pass

        stdout, stderr = process.communicate(input="\n", timeout=5)
        if process.returncode != 0:
            raise RuntimeError(f"Relay exited with {process.returncode}: {stderr}")

        received = [line for line in stdout.splitlines()
                    if line.startswith("Received message type ")]
        expected = [
            f"Received message type {i}, request {100 + i}, payload {len(payload)} bytes"
            for i, payload in enumerate(payloads)
        ]
        if received != expected:
            raise AssertionError(f"Expected:\n{expected}\nReceived:\n{received}\n{stderr}")
        for name, line in zip(MESSAGE_TYPES, received):
            print(f"PASS {name}: {line}")
        print("PASS: all 9 messages received in order on one connection.")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--relay", type=Path,
        default=Path(__file__).resolve().parent / "cmake-build-debug" / "Relay",
        help="Path to the built Relay executable",
    )
    args = parser.parse_args()
    try:
        run_test(args.relay.resolve())
    except (OSError, RuntimeError, AssertionError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

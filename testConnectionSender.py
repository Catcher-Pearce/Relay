import argparse
import socket
import sys

from testSessionMessages import PING, PONG, frame, read_message


def main():
    parser = argparse.ArgumentParser(description="Connect to a TCP listener and send a message.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12345)
    parser.add_argument("--message", default="Hello from Python!")
    args = parser.parse_args()

    try:
        print(f"Connecting to {args.host}:{args.port}...", flush=True)
        with socket.create_connection((args.host, args.port), timeout=5) as connection:
            print("Connected!", flush=True)
            payload = args.message.encode("utf-8")
            connection.sendall(frame(PING, 1, payload))
            response = read_message(connection)
            if response != (PONG, 1, payload):
                raise ValueError(f"Unexpected reply: {response}")
            print(f"Received PONG: {response[2].decode('utf-8')}", flush=True)
    except (OSError, AssertionError, ValueError) as error:
        print(f"Connection error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

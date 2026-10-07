import argparse
import socket
import sys


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
            connection.sendall((args.message + "\n").encode("utf-8"))
            print(f"Sent: {args.message}", flush=True)
    except OSError as error:
        print(f"Connection error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

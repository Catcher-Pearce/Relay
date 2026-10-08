"""Small Python peer for Relay's outgoing PING test."""
import socket

from testSessionMessages import PING, PONG, frame, read_message

with socket.socket() as server:
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 12345))
    server.listen()
    print("Waiting for Relay on 127.0.0.1:12345...", flush=True)
    while True:
        connection, address = server.accept()
        with connection:
            print("Relay connected:", address, flush=True)
            try:
                while True:
                    kind, request_id, payload = read_message(connection)
                    print(f"Received type {kind}, request {request_id}", flush=True)
                    if kind == PING:
                        connection.sendall(frame(PONG, request_id, payload))
            except (OSError, AssertionError) as error:
                print(f"Connection ended: {error}", flush=True)

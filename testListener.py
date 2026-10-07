import socket

with socket.socket() as server:
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 12345))
    server.listen()
    print("Waiting for Relay...", flush=True)

    while True:
        connection, address = server.accept()
        with connection:
            print("Relay connected:", address, flush=True)
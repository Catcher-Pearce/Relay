# Relay networking exercise

Relay currently accepts and initiates TCP connections and exchanges framed PING/PONG messages. It has no authentication, file transfer, discovery, or GUI.

Build with CMake, a C++20 compiler, Boost headers, and platform thread support:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug -j 2
```

Run a listener in one terminal and a client in another:

```sh
./cmake-build-debug/Relay --listen 12345
./cmake-build-debug/Relay --connect 127.0.0.1 12345
```

Use the listener computer's LAN IP instead of `127.0.0.1` to test two computers. The outgoing connection automatically sends one PING. Press Enter in either process to stop it. Running without arguments listens on port 12345.

## Responsibilities

- `ConnectionManager` owns the context, its worker thread, and the session registry. It creates every session through `addConnection()` and removes disconnected sessions. Registry keys are local connection numbers, not peer identities.
- `ConnectionListener` owns the acceptor and schedules one accept at a time. It calls the manager directly with each accepted socket.
- `ConnectionSender` asynchronously connects a socket and passes it to the same manager method. It sends a PING through the resulting session.
- `ConnectionSession` owns one socket, receives framed messages, and serializes outgoing writes through a queue.
- `MessageHandler.cpp` responds to PING with PONG and logs PONG. It receives only the current session and message, with no access to the registry.

The listener and sender hold direct references to the manager. Sessions have no manager reference or registry access. Each session receives one close-notification callback, supplied by the manager, which removes its registry entry. Lambdas passed to Asio are completion callbacks: the worker executes them when the associated network operation completes.

## Frame format

The existing 13-byte header is preserved:

| Field | Bytes |
| --- | --- |
| Type: PING = 0, PONG = 1 | 1 |
| Request ID | 8 |
| Payload length | 4 |
| Payload | Specified length |

Integers use big-endian order. PONG echoes the PING's request ID and payload. There is no pending-request table. Unknown types and payloads over 1 MiB close that connection.

## Incoming flow

1. Main creates the manager. Its worker enters `io_context.run()`. A work guard keeps the worker waiting even when there is no network operation yet.
2. Main calls `establishListener()`, which opens the port and schedules `async_accept()`.
3. A client connects. The worker runs the accept callback, which calls `manager.addConnection(socket)` and schedules the next accept.
4. The manager creates a shared session, stores it in the registry, and calls `start()`.
5. The session asynchronously reads exactly 13 header bytes, validates them, and reads the payload.
6. The body callback constructs a message and calls `handleMessage(session, message)`.
7. For PING, the handler calls `session.send(PONG)`. For PONG, it logs the response without replying.
8. The body callback schedules the next header read.

## Outgoing flow

1. Main calls `sendConnectionRequest(IP, port)`. The manager posts that operation onto the network thread.
2. The sender schedules `async_connect()`. Its callback runs on the worker when connecting finishes.
3. On success, it calls the same `manager.addConnection(socket)` used by the listener.
4. The new session starts its read loop, and the sender calls `session.send(PING)`.
5. `send()` encodes the message and appends a frame to the write queue. If the queue was empty, it starts `async_write()`.
6. The write callback removes the finished frame and starts the next queued write, if any. Only one write is active at a time.
7. The session reads the peer's PONG using the same header/body loop as incoming sessions.

The registry owns established sessions. Each asynchronous operation also captures a shared pointer to its session so closing and registry removal cannot destroy an object while an operation still uses it. Writes capture their frame to keep its bytes alive even if closing clears the queue.

On read/write failure, the session closes its socket and calls its close-notification callback to remove its registry entry. On program shutdown, the manager stops and joins its worker, then closes remaining sessions. This is immediate shutdown; queued messages are not guaranteed to finish.

Session methods and the registry are used only on the single network thread. Shutdown also accesses them after joining that thread, when callbacks can no longer run. Main uses only the manager's startup methods.

## Tests

```sh
python3 testSessionMessages.py
```

The test launches its own Relay processes on automatically allocated ports. It tests incoming and outgoing connections, split headers, binary and empty payloads, a burst of queued responses, echoed request IDs, disconnections, and invalid frames.

For manual tests, `testConnectionSender.py` sends a framed PING to a running Relay listener. `testListener.py` runs a Python peer that replies to Relay's outgoing PING.

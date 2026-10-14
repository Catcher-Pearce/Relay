# Relay

Relay is a C++20 application for sharing files between computers over TCP. The current CLI supports peer connections, Ping/Pong, and requesting a JSON directory listing. File-transfer methods and part of their message handling are implemented, but downloading a file through the application is not yet complete.

## Implemented

- Listening for incoming TCP connections and initiating outgoing connections.
- A persistent device ID in `deviceid.txt`, relative to the process's working directory.
- Hello exchange on both ends, pending-session registration, and identification by peer ID.
- Connection selection: the larger device ID keeps its outgoing connection; the smaller ID keeps its incoming connection. Both sides are expected to initiate connections.
- Framed messages, asynchronous reads, and a queue that serializes socket writes.
- CLI commands for Ping, peer listing, and filesystem listing requests.
- Home-folder creation and recursive JSON listings through `FolderManager`.
- Session-owned file transfers, indexed by request ID.
- File sending methods for metadata, 64 KiB chunks, and completion messages. The first chunk is queued automatically; subsequent chunks currently require manual advancement.
- File receiving methods for temporary files, chunk validation, finalization, and cancellation.

### Not yet complete

- `FileTransferComplete` and `FileTransferError` receive handlers. These message types are currently ignored by the dispatcher, even though the corresponding transfer methods exist.
- Automatic chunk advancement from socket write completion (see TODOs below).
- Automatic transfer cancellation and temporary-file cleanup when a session disconnects.
- Peer discovery and reconnect tracking for each peer. The CLI retries only its startup address while there are no established or pending connections.
- Authentication, encryption, a GUI, transfer resume, and checksums.

## Build and run

Requires a C++20 compiler, CMake 3.20+, Boost headers 1.70+, Qt 6 Core development files, and platform thread support. No Qt GUI module is required.

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug -j 2
```

Every instance listens and initiates a connection:

```text
Relay PEER_IP [LOCAL_PORT [PEER_PORT]]
```

The local port defaults to `12345`. The peer port defaults to the local port. To connect two computers, run this on each with the other's LAN address:

```sh
./cmake-build-debug/Relay OTHER_COMPUTER_IP
```

The listener remains active after a connection is established. Outgoing retries occur approximately once per second while all connection registries are empty.

### CLI commands

| Command | Action |
| --- | --- |
| `ping [PEER_ID]` | Sends an empty Ping and prints the Pong response. |
| `files [PEER_ID]` | Requests and prints the peer's shared-folder JSON listing. |
| `download "PATH" [PEER_ID]` | Requests a file relative to the peer's shared folder. Quote paths containing spaces. Saves under `HOME/relay/relay-downloads`. |
| `peers` | Prints connected device IDs and their remote endpoints. |
| `help` | Prints available commands. |
| `quit` or `exit` | Stops the application. EOF also exits. |

With exactly one connected peer, its ID can be omitted. With multiple peers, specify an ID from `peers`. The `files` command lists files; use `download "photos/my image.png"` to request one. Payload paths are UTF-8; use `/` separators.

### Testing two local instances

Use different working directories so the instances load different device IDs. In two terminals, replacing `/absolute/path/to/Relay` with the repository path:

```sh
# Terminal A
mkdir -p /tmp/relay-a
cd /tmp/relay-a
/absolute/path/to/Relay/cmake-build-debug/Relay 127.0.0.1 12345 12346
```

```sh
# Terminal B
mkdir -p /tmp/relay-b
cd /tmp/relay-b
/absolute/path/to/Relay/cmake-build-debug/Relay 127.0.0.1 12346 12345
```

After connecting, try `peers`, `ping`, and `files`. Each peer should retain one connection to the other. Two instances running as the same OS user use the same default shared/download folders.

## Folders and listings

Constructing the default `FolderManager` creates these directories if missing. Qt resolves the user's home folder on Windows and Linux:

```text
HOME/relay/
├── relay-shared/
└── relay-downloads/
```

Only `relay-shared` is listed and offered as a source for transfers. Downloads use `relay-downloads`. There is no automatic migration of files from older folder locations.

`getFileSystem()` returns a `QByteArray` containing compact UTF-8 JSON:

```json
{"entries":[{"path":"notes.txt","type":"file","size":5},{"path":"photos","type":"directory"}]}
```

Entries are sorted by relative path and include hidden files and empty directories. Paths use `/` separators. Types are `file`, `directory`, `symlink`, or `other`; only regular files have a `size` field, in bytes. Symbolic links are listed without following them. File contents are not embedded in this listing.

## Message frame

Every message uses a 13-byte header followed by the payload:

| Byte offset | Field | Length |
| --- | --- | --- |
| 0 | Message type | 1 byte |
| 1 | Request ID | 8 bytes, unsigned, big-endian |
| 9 | Payload length | 4 bytes, unsigned, big-endian |
| 13 | Payload | Exactly the advertised number of bytes |

Payloads are limited to 1 MiB per frame. An undefined type (including retired values 6 and 9), an oversized payload, or any non-Hello message received before identification closes the connection. Defined types without handlers are accepted at the framing layer and then ignored.

Request IDs correlate requests and responses. For file transfers, the request ID is also the transfer ID on every related frame. IDs must be unique among active transfers within a session. The single transfer map currently has no separate incoming/outgoing ID namespace, so both peers independently choosing the same active ID causes a collision.

JSON and path payloads use UTF-8 without a terminating null byte. Binary file data is sent unchanged. JSON sizes are nonnegative signed 64-bit integers, up to `9,223,372,036,854,775,807`.

### Message types

| Value | Type | Payload structure | Current handling |
| --- | --- | --- | --- |
| 0 | `Ping` | Arbitrary bytes, including empty. | Replies with Pong using the same ID and bytes. CLI sends empty payloads. |
| 1 | `Pong` | Exact copy of the Ping payload. | Logs the request ID. |
| 2 | `Hello` | Device ID as string bytes. | Sent automatically with ID `1` at session startup. Identifies the peer and registers or rejects the session. |
| 3 | `FileSystemRequest` | Empty by convention; the handler ignores payload contents. | Returns a listing with the same request ID. |
| 4 | `FileSystemResponse` | JSON: `{"entries":[...]}` as described above. | Prints the payload as text. |
| 5 | `FileRequest` | Raw UTF-8 relative path, e.g. `photos/image.png`; not JSON. | Rejects empty/null-containing paths, registers a sender, and starts the transfer. |
| 7 | `FileTransferStart` | JSON: `{"path":"photos/image.png","size":2048}`. | Validates metadata, registers a receiver, and opens its `.part` file. |
| 8 | `FileChunk` | 8-byte unsigned big-endian file offset, then 1–65,536 file-data bytes. | Decodes the offset, validates the incoming transfer, and writes file data. Errors return FileTransferError. |
| 10 | `FileTransferComplete` | Empty. | Sender emits it; receive handler still needs to call `finishReceive()`. |
| 11 | `FileTransferError` | JSON: `{"error":"description"}`. | Emitted for handled transfer failures and cancellation; receive handler is not wired yet. |

Hello is identification, not authentication. Empty peer IDs and later Hello messages changing an established ID close the connection; repeated Hello with the same ID is ignored.

### File-transfer lifecycle

The transfer protocol is:

```text
Receiver                         Sender
   | -------- FileRequest -------> |
   | <---- FileTransferStart ----- |
   | <--------- FileChunk -------- |
   | <--------- FileChunk -------- |
   | <--- FileTransferComplete ---- |
```

The sender currently queues metadata and the first chunk. The next chunk should be queued when the previous chunk's `async_write` finishes successfully; that callback is not implemented yet. Tests advance the sender manually. There are no application-level chunk acknowledgments. TCP handles reliable, ordered byte delivery. An empty file queues start and completion without data chunks.

The offset in a FileChunk is its starting byte position, not its size. It remains in the protocol to validate sequential writes. Chunk size is the frame payload length minus eight bytes.

The receiving methods require sequential offsets and reject data exceeding the advertised total. `receiveChunk()` takes the transfer ID, decoded offset, and a span containing only file data; its caller must strip the first eight payload bytes. A successful write is flushed before updating progress. Flush does not guarantee durable disk storage.

`beginReceive()` rejects rooted paths, `..`, symbolic-link components, and existing final or temporary destinations. A download such as `photos/image.png` is written to `photos/image.png.part`. `finishReceive()` checks the byte count and actual file size, closes the stream, and renames the temporary file. Path checks and file operations are separate and assume local filesystem entries are not changed concurrently.

`cancel()` closes streams, removes only its owned temporary file, and sends a transfer error if connected. Repeated cancellation and cancellation after completion do nothing. Destroying a transfer closes streams but does not remove its `.part` file. The sender's Completed state means completion was queued locally; there is no acknowledgment confirming that the receiver finalized its file.

## TODO: Advance chunks after socket writes

"Socket clear" means the queued frame's `async_write` has completed successfully. It does not mean the remote application saved the bytes, and the entire connection queue does not need to be empty. Use completion notifications rather than polling socket state.

1. **Implemented: per-frame callbacks to `ConnectionSession::send()`.** Accept an optional completion callback and store it alongside the encoded frame. Ping and other callers can omit it.
2. **Implemented: invoke callbacks in `writeNext()`.** Remove the completed frame, then notify its callback once. Handle callbacks that queue another frame without starting overlapping writes. Never advance a transfer after a write error or close; discard or fail outstanding callbacks.
3. **Continue from the chunk callback.** In `FileTransfer::sendNextChunk()`, attach a callback capturing the transfer ID. The callback receives a `ConnectionSession&`; use it to find the transfer again and check Sending state before calling `sendNextChunk()`. Avoid capturing a raw transfer pointer because cancellation/removal can destroy it.
4. **Finish after the last chunk write.** Its callback calls `sendNextChunk()`, which queues FileTransferComplete. Remove the sender from the map after the completion frame's write finishes. Preserve metadata-before-chunk ordering and support empty files.
5. **Bound outstanding file data.** Keep at most one queued/in-flight chunk per transfer; do not replace acknowledgments with a loop that queues the entire file. Add a connection-wide bound if many concurrent transfers are allowed.
6. **Wire receiving and failure cleanup.** FileChunk handling is implemented. Add a FileTransferComplete handler calling `finishReceive()`. Handle FileTransferError and disconnects by cancelling and removing transfers. Catch errors inside completion callbacks so they cannot escape the network worker.
7. **Test automatic advancement.** Verify a multi-chunk file completes without remote chunk acknowledgments, queued memory stays bounded, and cancellation/disconnection prevents callbacks from accessing removed transfers.

A future final saved-file confirmation can be added if the sender needs to know the receiver successfully finalized the download. Local write completion alone does not provide that confirmation.

## Code responsibilities

| Component | Responsibility |
| --- | --- |
| `ConnectionManager` | Owns the network worker, pending sessions keyed by local connection ID, and established sessions keyed by peer ID. Offers thread-safe posting for snapshots and sends. |
| `ConnectionListener` | Accepts sockets and registers them as incoming sessions. |
| `ConnectionSender` | Connects sockets and registers them as outgoing sessions. |
| `ConnectionSession` | Owns a socket, frame buffers, write queue, and `unique_ptr<FileTransfer>` map. Sends Hello and dispatches received frames. |
| `MessageHandler` | A short dispatcher calling separate named handlers for implemented message types. |
| `FolderManager` | Creates the shared/download directories and produces directory-listing JSON. |
| `FileTransfer` | Reads and queues outgoing chunks; validates and writes incoming chunks; tracks progress and performs explicit finalization/cancellation. Holds a weak session reference. |
| `DeviceIdentity` | Loads or creates the persistent UUID in `deviceid.txt`. |

Networking callbacks, registries, sessions, and transfers belong to the single network thread. File operations currently run synchronously on that thread. On shutdown, the manager stops and joins the worker before closing sessions; queued messages are not guaranteed to finish.

Transfer handlers catch standard exceptions and send JSON errors. There is no overall exception boundary around the worker; an uncaught callback exception can terminate the application. Filesystem listing errors currently have no handler-level catch.

## Automated tests

```sh
ctest --test-dir cmake-build-debug --output-on-failure
```

Tests require permission to open localhost sockets:

| Test | Coverage |
| --- | --- |
| `simultaneous_connections` | Both peers initiate connections, retain the same one socket, reject duplicate connections, and empty pending registries. |
| `folder_listing` | Directory creation, download exclusion, nested/hidden/Unicode entries, empty folders, symlinks, stable JSON, and missing-root errors. |
| `file_transfer_send` | Binary sending/receiving methods, offsets, absence of chunk acknowledgment frames, metadata errors, session transfer maps, empty files, finalization, cancellation, collisions, and disconnect errors. |

Transfer tests exercise methods and currently wired handlers; passing them does not imply a complete application-level download. The older root-level Python scripts target an earlier CLI/protocol and need updating before use.

# Relay

Relay is a C++20 application for sharing files between computers over TCP. The CLI supports peer connections, Ping/Pong, directory listings, and downloads. A separate background core serves a Qt Widgets GUI with peer file trees and download controls.

## Implemented

- Listening for incoming TCP connections and initiating outgoing connections.
- A persistent device ID in `deviceid.txt`, relative to the process's working directory.
- Hello exchange on both ends, pending-session registration, and identification by peer ID.
- Connection selection: the larger device ID keeps its outgoing connection; the smaller ID keeps its incoming connection. Both sides are expected to initiate connections.
- Framed messages, asynchronous reads, and a queue that serializes socket writes.
- CLI commands for Ping, peer listing, and filesystem listing requests.
- Home-folder creation and recursive JSON listings through `FolderManager`.
- Session-owned file transfers, indexed by request ID.
- File sending methods for metadata, 64 KiB chunks, and completion messages. Each successful chunk socket write automatically queues the next chunk.
- File receiving methods for temporary files, chunk validation, finalization, and cancellation.

### Not yet complete

- Automatic transfer cancellation and temporary-file cleanup when a session disconnects.
- Discovery filtering for already connected/pending peers and reconnect tracking per peer.
- Authentication, encryption, transfer resume, and checksums.

## Build and run

Requires a C++20 compiler, CMake 3.20+, Boost headers 1.70+, Qt 6 Core and Network development files, OpenSSL 3 development files, and platform thread support. The desktop target also requires Qt 6 Widgets. Pass `-DRELAY_BUILD_GUI=OFF` to build the core and CLI without the desktop GUI.

On Fedora, install the OpenSSL headers and link libraries with `sudo dnf install openssl-devel`.
On Windows, install OpenSSL for your compiler/architecture and pass
`-DOPENSSL_ROOT_DIR="path/to/openssl"` to CMake if it is not found automatically.
`RelayCore` links `OpenSSL::Crypto` for key/certificate APIs and `OpenSSL::SSL` for TLS.
This build dependency does not enable encryption on the existing TCP sessions;
certificate trust and TLS socket handshakes still need implementing.

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug -j 2
```

Start Relay without arguments on each computer:

```sh
./cmake-build-debug/Relay
```

TCP port `12345` is defined once in `main` and passed to `ConnectionManager`, which stores it for the listener and discovery advertisement. Starting the listener also starts UDP discovery on port `12346`. Instances broadcast periodically and connect to discovered peers using their source IP and advertised TCP port.

There is no command-line peer IP, startup TCP request, or manual retry loop. The listener and discovery remain active while the CLI accepts commands. Use `peers` to check discovered connections.

### Background core and Qt Widgets desktop application

Run `./cmake-build-debug/RelayGui` or select `RelayGui` in CLion. The GUI
attaches to the per-user `RelayDaemon`, starting it as a detached process when
needed. Closing the window leaves discovery, connections, and downloads running.
Reopen the GUI whenever needed; it restores current peers and recent download
results from the core. Multiple GUI windows can attach to the same core.

Click a connected peer to load its shared folder tree. Expand directories and
use **Download** beside a regular file. Symlinks and other special entries have
no download button. The Downloads table shows pending, completed, and failed
requests, including errors such as an existing destination file. Downloads retain
their relative folder structure under `HOME/relay/relay-downloads`. The GUI does
not overwrite existing files. **Refresh files** requests a fresh peer listing.

The core can also run without ever opening a GUI:

```sh
./cmake-build-debug/RelayDaemon --background
./cmake-build-debug/RelayGui
./cmake-build-debug/RelayDaemon --stop
```

Run `RelayDaemon` without `--background` to keep it in the foreground for debugging
or a process supervisor. This does not install a login/startup service. The core
continues until explicitly stopped, terminated, or the user session ends. Recent
download history is held in memory while the core runs; it is not restored after
a core restart. Resume and progress percentages are not implemented.

The GUI uses a user-restricted local socket (Unix socket on Linux, named pipe on
Windows); it owns no peer TCP sockets or file-transfer objects. It can reconnect
when the core becomes available again. `RelayDaemon --help` describes optional
`--data-dir`, `--socket`, `--port`, and `--no-discovery` settings. The GUI supports
`--socket` and `--no-start-core` to attach to a specifically managed core.

The daemon stores `deviceid.txt` and `devicename.txt` under Qt's per-user
`AppLocalDataLocation` for Relay (`~/.local/share/Relay` on a default Linux setup).
On its first launch from an existing Relay working directory, it copies existing
identity files into that location. Subsequent launches use that stable location.
`--data-dir` overrides it. The CLI still uses identity files in its working
directory. Both use TCP port `12345` by default, so stop the CLI before starting
the daemon on the same computer. The GUI's detached launch logs network/startup
diagnostics to `daemon.log` in the data directory. Saving the device name updates
subsequent discovery broadcasts without restarting the core.

The GUI/core integration test uses a temporary home directory, a private control
socket, a free TCP port, and a mock peer with discovery disabled. It verifies
nested file browsing, binary downloads completed with no GUI open, restored
results after reopening, and rejection of a duplicate daemon.

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

### Testing locally

The CLI listens on the fixed TCP port `12345`, so run it on two different computers for manual testing. For a same-machine test, use `ctest`: the connection test supplies port `0` to each manager to allocate distinct free ports and uses explicit peer identities. The existing connection test needs discovery isolation now that listener startup also starts discovery.

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

## TLS credential files

Before opening the listener, `main` calls `Authenticator::ensureCredentials()` with
the persistent device ID. On first start it creates these PEM files:

```text
HOME/relay/tls/
├── private-key.pem
├── public-key.pem
└── certificate.pem
```

The private key is EC P-256. The self-signed certificate uses SHA-256, is valid for
one year, and includes client/server usage and a `urn:relay:device:...` URI SAN.
The private key is unencrypted so Relay can start unattended. File creation uses
atomic saves and a credentials-directory lock. POSIX permissions restrict the
directory and private key to their owner; Windows relies on the user's directory
ACLs in addition to Qt's file-permission support. These files stay outside the
shared and download directories.

Later starts validate and reuse existing files. Missing public-key or certificate
files can be recreated from the existing private key. Missing private keys,
corrupt/mismatched files, different device IDs, or expired certificates cause an
error rather than silently replacing an identity. To renew an expired certificate,
remove only `certificate.pem` after stopping Relay; the next start reuses the key.
Changing `deviceid.txt` while retaining these files also causes an identity mismatch.

This prepares credentials; the current TCP sockets still do not use TLS.
Self-signed certificates need an explicit peer-trust/pinning scheme, and a device
URI certificate is not a substitute for IP/DNS hostname verification.

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

The sender queues metadata and the first chunk. After each chunk's `async_write` succeeds, its callback finds the session-owned transfer by ID and queues the next chunk. After the last chunk write, the next call queues completion. There are no application-level chunk acknowledgments. TCP handles reliable, ordered byte delivery. An empty file queues start and completion without data chunks.

The offset in a FileChunk is its starting byte position, not its size. It remains in the protocol to validate sequential writes. Chunk size is the frame payload length minus eight bytes.

The receiving methods require sequential offsets and reject data exceeding the advertised total. `receiveChunk()` takes the transfer ID, decoded offset, and a span containing only file data; its caller must strip the first eight payload bytes. A successful write is flushed before updating progress. Flush does not guarantee durable disk storage.

`beginReceive()` rejects rooted paths, `..`, symbolic-link components, and existing final or temporary destinations. A download such as `photos/image.png` is written to `photos/image.png.part`. `finishReceive()` checks the byte count and actual file size, closes the stream, and renames the temporary file. Path checks and file operations are separate and assume local filesystem entries are not changed concurrently.

`cancel()` closes streams, removes only its owned temporary file, and sends a transfer error if connected. Repeated cancellation and cancellation after completion do nothing. Destroying a transfer closes streams but does not remove its `.part` file. The sender's Completed state means completion was queued locally; there is no acknowledgment confirming that the receiver finalized its file.

## Socket-write progression and remaining TODOs

"Socket clear" means the queued frame's `async_write` has completed successfully. It does not mean the remote application saved the bytes, and the entire connection queue does not need to be empty. Use completion notifications rather than polling socket state.

1. **Implemented: per-frame callbacks to `ConnectionSession::send()`.** Accept an optional completion callback and store it alongside the encoded frame. Ping and other callers can omit it.
2. **Implemented: invoke callbacks in `writeNext()`.** Remove the completed frame, then notify its callback once. Handle callbacks that queue another frame without starting overlapping writes. Never advance a transfer after a write error or close; discard or fail outstanding callbacks.
3. **Implemented: continue from the chunk callback.** In `FileTransfer::sendNextChunk()`, attach a callback capturing the transfer ID. The callback receives a `ConnectionSession&`; use it to find the transfer again and check Sending state before calling `sendNextChunk()`. Avoid capturing a raw transfer pointer because cancellation/removal can destroy it.
4. **Finish after the last chunk write.** Its callback calls `sendNextChunk()`, which queues FileTransferComplete. Remove the sender from the map after the completion frame's write finishes. Preserve metadata-before-chunk ordering and support empty files.
5. **Bound outstanding file data.** Keep at most one queued/in-flight chunk per transfer; do not replace acknowledgments with a loop that queues the entire file. Add a connection-wide bound if many concurrent transfers are allowed.
6. **Wire receiving and failure cleanup.** FileChunk handling is implemented. Add a FileTransferComplete handler calling `finishReceive()`. Handle FileTransferError and disconnects by cancelling and removing transfers. Catch errors inside completion callbacks so they cannot escape the network worker.
7. **Extend callback tests.** Automatic multi-chunk progression and callbacks queueing additional writes are tested. Add explicit cancellation/disconnection tests while registered chunk callbacks are pending, and verify queue bounds across many transfers.

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

## Tests

The suite uses **Qt Test**, with fixtures, assertion failures that include source
locations, and named data rows. CMake registers each test method separately with
CTest so a failure identifies a specific behavior rather than a large test program.
The Qt 6 Test development component is required when `BUILD_TESTING` is enabled.
It is not required for a production-only build (`-DBUILD_TESTING=OFF`).

```sh
cmake -S . -B cmake-build-debug -DBUILD_TESTING=ON
cmake --build cmake-build-debug -j 2

# Unit tests: no sockets or network permissions required.
ctest --test-dir cmake-build-debug -L unit --output-on-failure

# Integration tests: use localhost sockets on automatically allocated ports.
ctest --test-dir cmake-build-debug -L integration --output-on-failure

# All tests, or one named behavior.
ctest --test-dir cmake-build-debug --output-on-failure
ctest --test-dir cmake-build-debug -R prematureCompletion --output-on-failure
```

| Location | Coverage |
| --- | --- |
| `Tests/Unit/AuthenticatorTest.cpp` | First-start credential generation, TLS-context loading, reuse, partial files, mismatches, expiry, and private-key permissions. |
| `Tests/Unit/DeviceIdentityTest.cpp` | ID persistence, explicit IDs, persisted names, Unicode, and save failures. |
| `Tests/Unit/FolderManagerTest.cpp` | Folder creation, listing content/order, hidden and Unicode files, symlinks, missing folders, and download exclusion. |
| `Tests/Unit/FileTransferTest.cpp` | States, unsafe paths, collisions, chunk validation, finalization, cancellation, and disconnected sessions without opening sockets. |
| `Tests/Unit/ConnectionSessionTest.cpp` | Transfer ownership and lookup, duplicate/null IDs, and close notification. |
| `Tests/Integration/MessageHandlerTest.cpp` | Real session framing, malformed messages, binary receive/finalization, and peer-error cleanup. |
| `Tests/Integration/SessionWriteTest.cpp` | Queued write ordering, callback behavior, cancelled transfers, and automatic binary/empty/multi-chunk sends. |
| `Tests/Integration/ConnectionManagerTest.cpp` | One-sided connections, simultaneous connection selection, duplicate rejection/replacement, and disconnect cleanup. |

Each fixture uses a fresh temporary directory. Identity tests restore the working
directory after each case; tests do not modify the user's device identity or home
folders. Integration tests disable discovery and never broadcast onto the LAN.
Network reads and CTest processes have timeouts. Symlink cases explicitly report
skips when the OS does not permit creating symlinks.

Run a Qt Test executable directly to select an individual data row, for example:

```sh
./cmake-build-debug/FileTransferTest 'unsafeDestination:parent-traversal'
./cmake-build-debug/SessionWriteTest 'sendsFileAutomatically:multiple-chunks'
```

The older standalone C++ test programs and obsolete Python protocol scripts have
been replaced by these suites. Discovery broadcasts and a full download between
separate computers are not covered by these localhost tests.

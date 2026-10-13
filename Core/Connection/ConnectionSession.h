#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
#include <string>

/**
 * @brief Owns one TCP socket and exchanges framed messages with a remote peer.
 *
 * The manager assigns a local numeric connection ID at construction. The remote
 * peer ID is learned from Hello, which must precede incoming Ping or Pong messages.
 * Frames contain a 1-byte type, 8-byte request ID, 4-byte payload length, and payload;
 * multi-byte header fields use big-endian order. Payloads are limited to 1 MiB.
 *
 * Use shared ownership before starting asynchronous operations. All methods and
 * mutable state belong to the network thread, except close() during manager
 * teardown after the worker has stopped. Read/write callbacks retain the session.
 */
class ConnectionSession : public std::enable_shared_from_this<ConnectionSession> {
public:
    /**
     * @brief Wire message kinds: Ping requests a Pong, and Hello carries a device ID.
     */
    enum class MessageType : std::uint8_t { Ping = 0, Pong = 1, Hello = 2 };
    /**
     * @brief A decoded or outgoing message with a type, request ID, and raw payload.
     *
     * Ping replies preserve the request ID and payload. Hello encodes the peer ID
     * as string bytes without a terminating null character.
     */
    struct Message {
        MessageType type;
        std::uint64_t requestId = 0;
        std::vector<std::uint8_t> payload;
    };

    /**
     * @brief Takes ownership of a connected socket and stores lifecycle callbacks.
     * @param socket Connected TCP socket transferred into this session.
     * @param connectionId Local numeric ID assigned by the manager, independent of peer ID.
     * @param onClosed Callback invoked once after socket cleanup to remove registry ownership.
     * @param onHello Callback invoked once with the first valid peer ID to register the session.
     *
     * Construction does not start I/O. Callback owners must outlive the session's use
     * of them; callbacks may be empty.
     */
    ConnectionSession(boost::asio::ip::tcp::socket socket,
                      std::uint64_t connectionId, std::function<void()> onClosed,
                      std::function<void(const std::string&)> onHello);
    /**
     * @brief Sends the local device ID in Hello and starts the asynchronous read loop.
     * @throws std::runtime_error If a newly generated device ID cannot be persisted.
     * @note Call once on an open session already owned by a shared_ptr.
     */
    void start();
    /**
     * @brief Records the remote ID and invokes the manager's Hello callback once.
     * @param peerId Device ID decoded from the received Hello payload.
     *
     * An empty ID or a later Hello with a different ID closes the connection.
     * Repeated Hello with the same ID and calls on a closed session are ignored.
     * The callback may reject registration and close this session.
     */
    void identifyPeer(std::string peerId);
    /**
     * @brief Returns the immutable local numeric ID used to track this socket.
     * @return Manager-assigned connection ID, independent of the remote device ID.
     */
    std::uint64_t connectionId() const { return connectionId_; }
    /**
     * @brief Encodes and queues a message for asynchronous transmission in FIFO order.
     * @param message Message whose payload bytes are copied into the outgoing frame.
     *
     * Starts a write if the queue was idle. Calls after close are ignored; a payload
     * larger than 1 MiB closes the session. Returning does not mean the write completed.
     */
    void send(Message message);
    /**
     * @brief Closes the socket, clears queued frames, and notifies the manager once.
     *
     * Repeated calls have no effect. Outstanding I/O completes with cancellation;
     * callbacks keep their active buffers alive until completion.
     */
    void close();

private:
    // Preserve framing: type (1), request ID (8), payload length (4), payload.
    // Multi-byte fields use big-endian byte order.
    static constexpr std::size_t HeaderSize = 13;
    static constexpr std::size_t MaxMessageSize = 1024 * 1024;
    /**
     * @brief Reads exactly one frame header asynchronously and validates type and length.
     *
     * On success, decodes the big-endian fields and starts readBody(). An I/O error,
     * unknown message type, or payload length above 1 MiB closes the session.
     */
    void readHeader();
    /**
     * @brief Reads one payload asynchronously and dispatches the decoded message.
     * @param type Validated message kind from the header.
     * @param requestId Decoded request ID used to correlate messages.
     * @param payloadLength Validated byte count, including zero for an empty payload.
     *
     * Rejects non-Hello messages until the peer ID is known. Dispatches to the message
     * handler and starts the next header read unless dispatch closed the session.
     * Read errors close the session.
     */
    void readBody(MessageType type, std::uint64_t requestId, std::size_t payloadLength);
    /**
     * @brief Starts an asynchronous write of the first queued frame.
     *
     * Requires a nonempty queue with no write already active. Completion removes the
     * frame and starts the next queued write. Write errors close the session; callbacks
     * retain the frame even if close() clears the queue.
     */
    void writeNext();
    /**
     * @brief Performs idempotent socket cleanup and invokes the close callback once.
     * @param error Reason for closing; an empty error indicates an explicit close.
     *
     * Closes the socket, clears queued writes, and logs the closure. Errors other than
     * EOF and cancellation are also logged. Registry ownership may be released by the
     * callback, so callers must keep the session alive through this operation.
     */
    void finish(boost::system::error_code error);

    boost::asio::ip::tcp::socket socket_;
    std::uint64_t connectionId_;
    std::function<void()> onClosed_;
    std::function<void(const std::string&)> onHello_;
    std::string peerId_;
    bool closed_ = false;
    std::array<std::uint8_t, HeaderSize> readHeader_{};
    std::vector<std::uint8_t> readBuffer_;
    // Front is the active write. Empty -> nonempty starts the first write.
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> writeQueue_;
};

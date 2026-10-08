#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

// All session methods and mutable data belong to the single network thread.
class ConnectionSession : public std::enable_shared_from_this<ConnectionSession> {
public:
    enum class MessageType : std::uint8_t { Ping = 0, Pong = 1 };
    struct Message {
        MessageType type;
        std::uint64_t requestId = 0;
        std::vector<std::uint8_t> payload;
    };

    ConnectionSession(boost::asio::ip::tcp::socket socket,
                      std::uint64_t connectionId, std::function<void()> onClosed);
    void start();
    void send(Message message);
    void close();

private:
    // Preserve framing: type (1), request ID (8), payload length (4), payload.
    // Multi-byte fields use big-endian byte order.
    static constexpr std::size_t HeaderSize = 13;
    static constexpr std::size_t MaxMessageSize = 1024 * 1024;
    void readHeader();
    void readBody(MessageType type, std::uint64_t requestId, std::size_t payloadLength);
    void writeNext();
    void finish(boost::system::error_code error);

    boost::asio::ip::tcp::socket socket_;
    std::uint64_t connectionId_;
    std::function<void()> onClosed_;
    bool closed_ = false;
    std::array<std::uint8_t, HeaderSize> readHeader_{};
    std::vector<std::uint8_t> readBuffer_;
    // Front is the active write. Empty -> nonempty starts the first write.
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> writeQueue_;
};

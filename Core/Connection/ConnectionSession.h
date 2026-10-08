//
// Created by catcherpearce on 10/7/26.
//

#ifndef RELAY_CONNECTIONSESSION_H
#define RELAY_CONNECTIONSESSION_H

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include "ConnectionMap.h"
#include <memory>
#include <string>
#include <vector>

// Create sessions with std::make_shared; async callbacks retain shared_from_this().
// Session state and handlers belong to the manager's network thread. Public
// start/send/close operations must dispatch onto the socket's executor.
class ConnectionSession : public std::enable_shared_from_this<ConnectionSession> {
public:
    enum class State {
        Authenticating,
        Ready,
        Closing,
        Closed
    };

    enum class MessageType : std::uint8_t {
        Ping,
        Pong,
        Authentication,
        FileListRequest,
        FileListResponse,
        FileRequest,
        FileChunk,
        TransferComplete,
        Error
    };

    struct Message {
        MessageType type;
        std::uint64_t requestId = 0;
        std::vector<std::uint8_t> payload;
    };

    explicit ConnectionSession(boost::asio::ip::tcp::socket socket,
                               ConnectionMap& connections);

    ConnectionSession(const ConnectionSession&) = delete;
    ConnectionSession& operator=(const ConnectionSession&) = delete;

    void start();
    void send(Message message);
    void close();

    // Called on the network thread only after credentials have been verified.
    void markAuthenticated(std::string peerId);

    // Read these on the network thread; send snapshots to the UI.
    [[nodiscard]] const std::string& peerId() const;
    [[nodiscard]] State state() const;

private:
    // Wire format: type (1 byte), request ID (8), payload length (4), payload.
    // Multi-byte fields use big-endian order.
    static constexpr std::size_t HeaderSize = 13;
    static constexpr std::size_t MaxMessageSize = 1024 * 1024;
    static constexpr std::size_t MaxQueuedBytes = 8 * 1024 * 1024;

    void readHeader();
    void readBody(MessageType type, std::uint64_t requestId,
                  std::size_t payloadLength);
    void writeNext();
    void armAuthenticationTimeout();
    void finish(boost::system::error_code error);

    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer authenticationTimer_;
    std::string peerId_;
    State state_ = State::Authenticating;

    std::array<std::uint8_t, HeaderSize> readHeader_{};
    std::vector<std::uint8_t> readBuffer_;

    // Each frame owns its bytes until async_write completes. Only one write
    // may be active; captures must retain its frame even during shutdown.
    std::deque<std::shared_ptr<const std::vector<std::uint8_t>>> writeQueue_;
    std::size_t queuedBytes_ = 0;
    bool writeInProgress_ = false;

    ConnectionMap& connections_;
};


#endif //RELAY_CONNECTIONSESSION_H

//
// Created by catcherpearce on 10/7/26.
//

#include "ConnectionSession.h"
#include "MessageHandler.h"
#include <boost/asio.hpp>
#include <iostream>
#include <utility>

ConnectionSession::ConnectionSession(
    boost::asio::ip::tcp::socket socket,
    ConnectionMap& connections)
    : socket_(std::move(socket)),
      authenticationTimer_(socket_.get_executor()),
      connections_(connections)
{
}

void ConnectionSession::start() {
    auto self = shared_from_this();
    boost::asio::dispatch(socket_.get_executor(), [self] {
        if (self->state_ != State::Closed && self->state_ != State::Closing) {
            self->readHeader();
        }
    });
}

void ConnectionSession::readHeader() {
    auto self = shared_from_this();

    boost::asio::async_read(
        socket_,
        boost::asio::buffer(readHeader_),
        [self](boost::system::error_code error, std::size_t bytesRead) {
            if (error) {
                self->finish(error);
                return;
            }

            std::cout << "Received header: "
                      << bytesRead << " bytes\n";

            auto type = static_cast<MessageType>(self->readHeader_[0]);

            std::uint64_t requestId = 0;

            for (std::size_t i = 1; i <= 8; ++i) {
                requestId = (requestId << 8) | self->readHeader_[i];
            }

            std::uint32_t payloadLength = 0;

            for (std::size_t i = 9; i <= 12; ++i) {
                payloadLength = (payloadLength << 8) | self->readHeader_[i];
            }

            if (payloadLength > MaxMessageSize) {
                self->finish(boost::asio::error::message_size);
                return;
            }

            self->readBody(type, requestId, payloadLength);
        }
    );
}

void ConnectionSession::readBody(MessageType type, std::uint64_t requestId,
                  std::size_t payloadLength) {
    auto self = shared_from_this();
    readBuffer_.resize(payloadLength);

    boost::asio::async_read(
        socket_,
        boost::asio::buffer(readBuffer_),
        [self, type, requestId](boost::system::error_code error, std::size_t bytesRead) {
            if (error) {
                self->finish(error);
                return;
            }

            Message message {
                type,
                requestId,
                std::move(self->readBuffer_)
            };

            relay::handleMessage(self->connections_, *self, std::move(message));

            if (self->state_ != State::Closing &&
                self->state_ != State::Closed) {
                    self->readHeader();
        }});
}

void ConnectionSession::finish(boost::system::error_code error) {
    if (state_ == State::Closed) {
        return;
    }
    state_ = State::Closed;
    boost::system::error_code ignored;
    authenticationTimer_.cancel();
    socket_.close(ignored);
    // A duplicate session must never erase the original peer's connection.
    auto found = connections_.find(peerId_);
    if (found != connections_.end() && found->second.get() == this) {
        connections_.erase(found);
    }
    if (error && error != boost::asio::error::operation_aborted) {
        std::cerr << "Disconnected: " << error.message() << '\n';
    }
}

void ConnectionSession::close() {
    auto self = shared_from_this();
    boost::asio::dispatch(socket_.get_executor(), [self] {
        self->finish({});
    });
}

void ConnectionSession::markAuthenticated(std::string peerId) {
    // Authentication verification belongs to the authentication service.
    if (state_ != State::Authenticating || peerId.empty()) {
        return;
    }
    if (!connections_.emplace(peerId, shared_from_this()).second) {
        finish(boost::asio::error::already_connected);
        return;
    }
    peerId_ = std::move(peerId);
    state_ = State::Ready;
    authenticationTimer_.cancel();
}

const std::string& ConnectionSession::peerId() const {
    return peerId_;
}

ConnectionSession::State ConnectionSession::state() const {
    return state_;
}

#include "ConnectionSession.h"
#include "MessageHandler.h"
#include <boost/asio.hpp>
#include <iostream>
#include <utility>

ConnectionSession::ConnectionSession(boost::asio::ip::tcp::socket socket,
                                     std::uint64_t connectionId, std::function<void()> onClosed)
    : socket_(std::move(socket)), connectionId_(connectionId),
      onClosed_(std::move(onClosed)) {}

void ConnectionSession::start() {
    std::cout << "Connection " << connectionId_ << " established" << std::endl;
    readHeader();
}

void ConnectionSession::readHeader() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(readHeader_),
        [self](boost::system::error_code error, std::size_t) {
            if (error) {
                self->finish(error);
                return;
            }
            const auto rawType = self->readHeader_[0];
            if (rawType > static_cast<std::uint8_t>(MessageType::Pong)) {
                self->finish(boost::asio::error::invalid_argument);
                return;
            }
            std::uint64_t requestId = 0;
            for (std::size_t i = 1; i <= 8; ++i) {
                requestId = (requestId << 8) | self->readHeader_[i];
            }
            std::uint32_t length = 0;
            for (std::size_t i = 9; i < HeaderSize; ++i) {
                length = (length << 8) | self->readHeader_[i];
            }
            if (length > MaxMessageSize) {
                self->finish(boost::asio::error::message_size);
                return;
            }
            self->readBody(static_cast<MessageType>(rawType), requestId, length);
        });
}

void ConnectionSession::readBody(MessageType type, std::uint64_t requestId, std::size_t payloadLength) {
    readBuffer_.resize(payloadLength);
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(readBuffer_),
        [self, type, requestId](boost::system::error_code error, std::size_t) {
            if (error) {
                self->finish(error);
                return;
            }
            relay::handleMessage(*self, {type, requestId, std::move(self->readBuffer_)});
            if (!self->closed_) {
                self->readHeader();
            }
        });
}

void ConnectionSession::send(Message message) {
    if (closed_) {
        return;
    }
    if (message.payload.size() > MaxMessageSize) {
        finish(boost::asio::error::message_size);
        return;
    }
    auto frame = std::make_shared<std::vector<std::uint8_t>>(HeaderSize, 0);
    (*frame)[0] = static_cast<std::uint8_t>(message.type);
    for (std::size_t i = 0; i < 8; ++i) {
        (*frame)[1 + i] = static_cast<std::uint8_t>(message.requestId >> (56 - 8 * i));
    }
    auto length = static_cast<std::uint32_t>(message.payload.size());
    for (std::size_t i = 0; i < 4; ++i) {
        (*frame)[9 + i] = static_cast<std::uint8_t>(length >> (24 - 8 * i));
    }
    frame->insert(frame->end(), message.payload.begin(), message.payload.end());
    const bool idle = writeQueue_.empty();
    writeQueue_.push_back(std::move(frame));
    if (idle) {
        writeNext();
    }
}

void ConnectionSession::writeNext() {
    auto self = shared_from_this();
    auto frame = writeQueue_.front();
    boost::asio::async_write(socket_, boost::asio::buffer(*frame),
        [self, frame](boost::system::error_code error, std::size_t) {
            // The captured frame stays alive even if close() clears the queue.
            if (self->closed_) {
                return;
            }
            if (error) {
                self->finish(error);
                return;
            }
            self->writeQueue_.pop_front();
            if (!self->writeQueue_.empty()) {
                self->writeNext();
            }
        });
}

void ConnectionSession::close() {
    finish({});
}

void ConnectionSession::finish(boost::system::error_code error) {
    if (closed_) {
        return;
    }
    closed_ = true;
    boost::system::error_code ignored;
    socket_.close(ignored);
    writeQueue_.clear();
    std::cout << "Connection " << connectionId_ << " closed" << std::endl;
    if (error && error != boost::asio::error::eof &&
        error != boost::asio::error::operation_aborted) {
        std::cerr << "Connection " << connectionId_ << ": " << error.message() << std::endl;
    }
    if (onClosed_) {
        // Notify once, after cleanup. The callback may release registry ownership.
        auto notify = std::move(onClosed_);
        notify();
    }
}

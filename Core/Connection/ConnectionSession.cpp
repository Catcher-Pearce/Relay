#include "ConnectionSession.h"
#include "MessageHandler.h"
#include <boost/asio.hpp>
#include <iostream>
#include <utility>
#include <stdexcept>

ConnectionSession::ConnectionSession(boost::asio::ip::tcp::socket socket,
                                     ConnectionSession::Direction direction,
                                     std::uint64_t connectionId, std::function<void()> onClosed,
                                     std::function<void(const std::string&)> onHello)
    : socket_(std::move(socket)), connectionId_(connectionId), direction_(direction),
      onClosed_(std::move(onClosed)), onHello_(std::move(onHello)) {}

FileTransfer& ConnectionSession::addFileTransfer(std::uint64_t requestId,
                                                std::unique_ptr<FileTransfer> transfer) {
    if (closed_) throw std::logic_error("Cannot add a transfer to a closed session");
    if (!transfer) throw std::invalid_argument("File transfer must not be null");
    const auto [entry, inserted] = transfers_.try_emplace(requestId, std::move(transfer));
    if (!inserted) throw std::logic_error("A transfer with this request ID already exists");
    return *entry->second;
}

void ConnectionSession::start(const std::string& deviceId) {
    std::cout << "Connection " << connectionId_ << " established" << std::endl;
    send({MessageType::Hello, 1, {deviceId.begin(), deviceId.end()}});
    readHeader();
}

void ConnectionSession::removeFileTransfer(std::uint64_t requestId) {
    transfers_.erase(requestId);
}

FileTransfer* ConnectionSession::findFileTransfer(std::uint64_t requestId) {
    const auto entry = transfers_.find(requestId);
    return entry == transfers_.end() ? nullptr : entry->second.get();
}

void ConnectionSession::identifyPeer(std::string peerId) {
    if (closed_) {
        return;
    }
    if (peerId.empty() || (!peerId_.empty() && peerId_ != peerId)) {
        finish(boost::asio::error::invalid_argument);
        return;
    }
    if (!peerId_.empty()) {
        return;
    }
    peerId_ = std::move(peerId);
    if (onHello_) {
        auto notify = std::move(onHello_);
        notify(peerId_);
    }
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
            // Wire values 6 and 9 are retired and must not be accepted.
            if (rawType == 6 || rawType == 9 || rawType > static_cast<std::uint8_t>(MessageType::FileTransferError)) {
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
            if (self->peerId_.empty() && type != MessageType::Hello) {
                self->finish(boost::asio::error::invalid_argument);
                return;
            }
            relay::handleMessage(*self, {type, requestId, std::move(self->readBuffer_)});
            if (!self->closed_) {
                self->readHeader();
            }
        });
}

void ConnectionSession::send(Message message, std::function<void(ConnectionSession&)> onWritten) {
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
    writeQueue_.push_back({std::move(frame), std::move(onWritten)});
    if (!writeInProgress_) {
        writeNext();
    }
}

void ConnectionSession::writeNext() {
    if (closed_ || writeInProgress_ || writeQueue_.empty()) return;
    writeInProgress_ = true;
    auto self = shared_from_this();
    auto frame = writeQueue_.front().frame;
    auto onWritten = writeQueue_.front().onWritten;
    boost::asio::async_write(socket_, boost::asio::buffer(*frame),
        [self, frame, onWritten](boost::system::error_code error, std::size_t) {
            // The captured frame stays alive even if close() clears the queue.
            if (self->closed_) {
                return;
            }
            if (error) {
                self->finish(error);
                return;
            }
            self->writeQueue_.pop_front();

            // Keep the guard set: send() called by onWritten must only enqueue.
            try {
                if (onWritten) onWritten(*self);
            } catch (const std::exception& exception) {
                std::cerr << "Write callback failed: " << exception.what() << std::endl;
                self->finish(boost::asio::error::operation_aborted);
                return;
            } catch (...) {
                self->finish(boost::asio::error::operation_aborted);
                return;
            }

            self->writeInProgress_ = false;
            self->writeNext();
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
    writeInProgress_ = false;
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

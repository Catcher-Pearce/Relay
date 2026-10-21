#include "MessageHandler.h"
#include <iostream>
#include <utility>
#include <algorithm>
#include <stdexcept>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "FileShare/FileTransfer.h"
#include "FileShare/FolderManager.h"

namespace relay {
namespace {

void handlePing(ConnectionSession& session, ConnectionSession::Message& message) {
    std::cout << "Received PING, request " << message.requestId << std::endl;
    session.send({ConnectionSession::MessageType::Pong, message.requestId, std::move(message.payload)});
}

void handlePong(ConnectionSession&, ConnectionSession::Message& message) {
    std::cout << "Received PONG, request " << message.requestId << std::endl;
}

void handleHello(ConnectionSession& session, ConnectionSession::Message& message) {
    session.identifyPeer(std::string(message.payload.begin(), message.payload.end()));
}

void handleFileSystemRequest(ConnectionSession& session, ConnectionSession::Message& message) {
    try {
        FolderManager folders;
        const auto json = folders.getFileSystem();
        session.send({ConnectionSession::MessageType::FileSystemResponse,
                      message.requestId, {json.begin(), json.end()}});
    } catch (const std::exception& error) {
        const auto json = QJsonDocument(QJsonObject{{"error", QString::fromUtf8(error.what())}})
            .toJson(QJsonDocument::Compact);
        session.send({ConnectionSession::MessageType::FileTransferError,
                      message.requestId, {json.begin(), json.end()}});
    }
}

void handleFileSystemResponse(ConnectionSession& session, ConnectionSession::Message& message) {
    session.notifyFileEvent(message);
    std::cout << "Received file system response, request " << message.requestId << '\n'
              << std::string(message.payload.begin(), message.payload.end()) << std::endl;
}

void handleFileRequest(ConnectionSession& session, ConnectionSession::Message& message) {
    bool registered = false;
    try {
        if (message.payload.empty() ||
            std::find(message.payload.begin(), message.payload.end(), 0) != message.payload.end()) {
            throw std::invalid_argument("File request must contain a nonempty path without null bytes");
        }
        const std::u8string requestedPath(message.payload.begin(), message.payload.end());
        FolderManager folders;
        auto& transfer = session.addFileTransfer(message.requestId, std::make_unique<FileTransfer>(
            session.weak_from_this(), folders.sharedFolder(), folders.downloadsFolder()));
        registered = true;
        transfer.sendFile(message.requestId, std::filesystem::path(requestedPath));
        if (transfer.state() == FileTransfer::State::Completed) {
            session.removeFileTransfer(message.requestId);
        }
    } catch (const std::exception& error) {
        if (registered) session.removeFileTransfer(message.requestId);
        const auto json = QJsonDocument(QJsonObject{{"error", QString::fromUtf8(error.what())}})
            .toJson(QJsonDocument::Compact);
        session.notifyFileEvent({ConnectionSession::MessageType::FileTransferError, message.requestId,
                                 {json.begin(), json.end()}});
        session.send({ConnectionSession::MessageType::FileTransferError, message.requestId,
                      {json.begin(), json.end()}});
    }
}

void handleFileTransferStart(ConnectionSession& session, ConnectionSession::Message& message) {
    bool registered = false;
    try {
        const QByteArray payload(reinterpret_cast<const char*>(message.payload.data()),
                                 static_cast<qsizetype>(message.payload.size()));
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            throw std::invalid_argument("Transfer metadata must be a valid JSON object");
        }
        const auto metadata = document.object();
        const auto pathValue = metadata.value("path");
        const auto sizeValue = metadata.value("size");
        const auto expectedSize = sizeValue.toInteger(-1);
        if (!pathValue.isString() || pathValue.toString().isEmpty() ||
            pathValue.toString().contains(QChar::Null)) {
            throw std::invalid_argument("Transfer metadata must contain a nonempty path without null characters");
        }
        if (!sizeValue.isDouble() || expectedSize < 0) {
            throw std::invalid_argument("Transfer metadata size must be a nonnegative 64-bit integer");
        }
        if (session.findFileTransfer(message.requestId)) {
            throw std::invalid_argument("A transfer with this request ID already exists");
        }
        const auto relativePath = std::filesystem::path(pathValue.toString().toStdU16String());
        FolderManager folders;
        auto& transfer = session.addFileTransfer(message.requestId, std::make_unique<FileTransfer>(
            session.weak_from_this(), folders.sharedFolder(), folders.downloadsFolder()));
        registered = true;
        transfer.beginReceive(message.requestId, relativePath, static_cast<std::uint64_t>(expectedSize));
    } catch (const std::exception& error) {
        if (registered) session.removeFileTransfer(message.requestId);
        const auto json = QJsonDocument(QJsonObject{{"error", QString::fromUtf8(error.what())}})
            .toJson(QJsonDocument::Compact);
        session.notifyFileEvent({ConnectionSession::MessageType::FileTransferError, message.requestId,
                                 {json.begin(), json.end()}});
        session.send({ConnectionSession::MessageType::FileTransferError, message.requestId,
                      {json.begin(), json.end()}});
    }
}

void handleFileChunk(ConnectionSession& session, const ConnectionSession::Message& message) {
    auto* transfer = session.findFileTransfer(message.requestId);
    try {
        if (!transfer) throw std::invalid_argument("Unknown transfer ID in file chunk");
        if (transfer->state() != FileTransfer::State::Receiving) {
            throw std::invalid_argument("File chunk does not belong to an active incoming transfer");
        }
        constexpr std::size_t OffsetSize = 8;
        if (message.payload.size() <= OffsetSize ||
            message.payload.size() > OffsetSize + FileTransfer::ChunkSize) {
            throw std::invalid_argument("File chunk must contain an 8-byte offset and 1 to 65536 data bytes");
        }
        std::uint64_t offset = 0;
        for (std::size_t i = 0; i < OffsetSize; ++i) {
            offset = (offset << 8) | message.payload[i];
        }
        const auto data = std::span<const std::uint8_t>(message.payload).subspan(OffsetSize);
        // receiveChunk checks the ID, sequential offset, and remaining file size.
        transfer->receiveChunk(message.requestId, offset, data);
    } catch (const std::exception& error) {
        // Invalid messages leave the existing transfer unchanged. Disk/session
        // failures mark it Failed; clean up that transfer before removing ownership.
        if (transfer && transfer->state() == FileTransfer::State::Failed) {
            try {
                transfer->cancel(false);
                session.removeFileTransfer(message.requestId);
            } catch (const std::exception& cleanupError) {
                // Retain the failed transfer so its owned temporary file can be retried.
                std::cerr << "Transfer cleanup failed: " << cleanupError.what() << std::endl;
            }
        }
        const auto json = QJsonDocument(QJsonObject{{"error", QString::fromUtf8(error.what())}})
            .toJson(QJsonDocument::Compact);
        session.notifyFileEvent({ConnectionSession::MessageType::FileTransferError, message.requestId,
                                 {json.begin(), json.end()}});
        session.send({ConnectionSession::MessageType::FileTransferError, message.requestId,
                      {json.begin(), json.end()}});
    }
}

void handleFileTransferComplete(ConnectionSession& session, const ConnectionSession::Message& message) {
    auto* transfer = session.findFileTransfer(message.requestId);
    try {
        if (!message.payload.empty()) throw std::invalid_argument("Transfer complete message must have empty payload");

        if (!transfer) throw std::invalid_argument("Unknown transfer ID in file transfer complete message");

        transfer->finishReceive(message.requestId);

        session.removeFileTransfer(message.requestId);
        transfer = nullptr;

        session.notifyFileEvent(message);
        std::cout << "Download finished, transfer " << message.requestId << std::endl;
    } catch (const std::exception& error) {
        if (transfer && transfer->state() == FileTransfer::State::Failed) {
            try {
                transfer->cancel(false);
                session.removeFileTransfer(message.requestId);
            } catch (const std::exception& cleanupError) {
                std::cerr << "Transfer cleanup failed: " << cleanupError.what() << std::endl;
            }
        }
        const auto json = QJsonDocument(QJsonObject{{"error", QString::fromUtf8(error.what())}})
            .toJson(QJsonDocument::Compact);
        session.notifyFileEvent({ConnectionSession::MessageType::FileTransferError, message.requestId,
                                 {json.begin(), json.end()}});
        session.send({ConnectionSession::MessageType::FileTransferError, message.requestId,
                      {json.begin(), json.end()}});
    }
}


void handleFileTransferError(ConnectionSession& session, const ConnectionSession::Message& message) {
    try {
        const QByteArray payload(reinterpret_cast<const char*>(message.payload.data()),
                                 static_cast<qsizetype>(message.payload.size()));
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            throw std::invalid_argument("Transfer error must be a valid JSON object");
        }
        const auto error = document.object().value("error");
        if (!error.isString() || error.toString().isEmpty()) {
            throw std::invalid_argument("Transfer error must contain a nonempty error string");
        }
        std::cerr << "Peer reported an error for transfer " << message.requestId
                  << ": " << error.toString().toStdString() << std::endl;

        if (auto* transfer = session.findFileTransfer(message.requestId)) {
            transfer->cancel(false);
            session.removeFileTransfer(message.requestId);
        }
        session.notifyFileEvent(message);
        // Unknown IDs can occur after local cleanup; logging the peer's error is enough.
    } catch (const std::exception& error) {
        std::cerr << "Could not handle error for transfer " << message.requestId
                  << ": " << error.what() << std::endl;
    }
}

} // namespace

void handleMessage(ConnectionSession& session, ConnectionSession::Message message) {
    switch (message.type) {
    case ConnectionSession::MessageType::Ping:
        handlePing(session, message);
        break;
    case ConnectionSession::MessageType::Pong:
        handlePong(session, message);
        break;
    case ConnectionSession::MessageType::Hello:
        handleHello(session, message);
        break;
    case ConnectionSession::MessageType::FileSystemRequest:
        handleFileSystemRequest(session, message);
        break;
    case ConnectionSession::MessageType::FileSystemResponse:
        handleFileSystemResponse(session, message);
        break;
    case ConnectionSession::MessageType::FileRequest:
        handleFileRequest(session, message);
        break;
    case ConnectionSession::MessageType::FileTransferStart:
        handleFileTransferStart(session, message);
        break;
    case ConnectionSession::MessageType::FileChunk:
        handleFileChunk(session, message);
        break;
    case ConnectionSession::MessageType::FileTransferComplete:
        handleFileTransferComplete(session, message);
        break;
    case ConnectionSession::MessageType::FileTransferError:
        handleFileTransferError(session, message);
        break;
    default:
        break;
    }
}

} // namespace relay

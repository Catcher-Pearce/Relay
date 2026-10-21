#include "FileShare/FileTransfer.h"
#include "Connection/ConnectionSession.h"
#include "Connection/MessageHandler.h"
#include <boost/asio.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

using boost::asio::ip::tcp;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
ConnectionSession::Message readFrame(tcp::socket& socket) {
    std::array<std::uint8_t, 13> header{};
    boost::asio::read(socket, boost::asio::buffer(header));
    std::uint64_t id = 0;
    std::uint32_t size = 0;
    for (int i = 1; i < 9; ++i) id = (id << 8) | header[i];
    for (int i = 9; i < 13; ++i) size = (size << 8) | header[i];
    require(size <= FileTransfer::ChunkSize + 1024, "Unexpected frame size");
    ConnectionSession::Message message{static_cast<ConnectionSession::MessageType>(header[0]), id,
                                       std::vector<std::uint8_t>(size)};
    boost::asio::read(socket, boost::asio::buffer(message.payload));
    return message;
}
int main() {
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create test folder");
        const auto base = std::filesystem::path(temporary.path().toStdU16String());
        const auto shared = base / "shared";
        std::filesystem::create_directories(shared / "nested");
        std::string contents(FileTransfer::ChunkSize + 3, '\0');
        for (std::size_t i = 0; i < contents.size(); ++i) contents[i] = static_cast<char>(i % 256);
        std::ofstream(shared / "nested" / "file.bin", std::ios::binary).write(contents.data(), contents.size());
        std::ofstream(shared / "empty.bin", std::ios::binary);
        std::ofstream(base / "outside.bin", std::ios::binary) << "outside";
        boost::asio::io_context context;
        tcp::acceptor listener(context, tcp::endpoint(tcp::v4(), 0));
        tcp::socket sender(context), receiver(context);
        sender.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), listener.local_endpoint().port()));
        listener.accept(receiver);
        auto session = std::make_shared<ConnectionSession>(std::move(sender),
            ConnectionSession::Direction::Outgoing, 1, nullptr, nullptr);
        auto& transfer = session->addFileTransfer(42,
            std::make_unique<FileTransfer>(session, shared, base / "downloads"));
        bool duplicateRejected = false;
        try { session->addFileTransfer(42, std::make_unique<FileTransfer>(session, shared, base / "downloads")); }
        catch (const std::logic_error&) { duplicateRejected = true; }
        require(duplicateRejected, "Duplicate transfer ID was accepted");
        bool nullRejected = false;
        try { session->addFileTransfer(43, nullptr); }
        catch (const std::invalid_argument&) { nullRejected = true; }
        require(nullRejected, "Null transfer was accepted");
        auto flush = [&] { context.restart(); context.run(); };
        int callbackCount = 0;
        session->send({ConnectionSession::MessageType::Ping, 100, {}}, [&](ConnectionSession& current) {
            ++callbackCount;
            current.send({ConnectionSession::MessageType::Ping, 102, {}}, [&](ConnectionSession&) {
                ++callbackCount;
            });
        });
        session->send({ConnectionSession::MessageType::Ping, 101, {}}, [&](ConnectionSession&) {
            ++callbackCount;
        });
        require(callbackCount == 0, "Write callback fired before the socket write");
        flush();
        require(callbackCount == 3, "Write callbacks must run exactly once");
        for (const auto id : {100, 101, 102}) {
            require(readFrame(receiver).requestId == id, "Callback queueing changed frame order");
        }
        for (const auto& invalidPath : {std::vector<std::uint8_t>{}, std::vector<std::uint8_t>{'a', 0, 'b'}}) {
            relay::handleMessage(*session, {ConnectionSession::MessageType::FileRequest, 99, invalidPath});
            flush();
            const auto error = readFrame(receiver);
            require(error.type == ConnectionSession::MessageType::FileTransferError && error.requestId == 99,
                    "Malformed file request must return a transfer error");
        }
        for (const std::string metadata : {"not-json", "[]", "{}", "{\"path\":\"file.bin\",\"size\":-1}",
                "{\"path\":\"file.bin\",\"size\":1.5}", "{\"path\":\"file.bin\",\"size\":\"1\"}",
                "{\"path\":\"\",\"size\":0}", "{\"path\":\"file.bin\",\"size\":0}"}) {
            relay::handleMessage(*session, {ConnectionSession::MessageType::FileTransferStart, 42,
                                           {metadata.begin(), metadata.end()}});
            flush();
            const auto error = readFrame(receiver);
            require(error.type == ConnectionSession::MessageType::FileTransferError && error.requestId == 42,
                    "Invalid or duplicate start must return a transfer error");
            require(session->findFileTransfer(42) == &transfer, "Rejected start replaced existing transfer");
        }
        transfer.sendFile(42, "nested/file.bin");
        bool busy = false;
        try { transfer.sendFile(43, "empty.bin"); } catch (const std::logic_error&) { busy = true; }
        require(busy && transfer.state() == FileTransfer::State::Sending, "Active transfer must not be overwritten");
        flush();
        auto metadata = readFrame(receiver);
        require(metadata.type == ConnectionSession::MessageType::FileTransferStart && metadata.requestId == 42,
                "Missing transfer metadata");
        const auto object = QJsonDocument::fromJson(QByteArray(
            reinterpret_cast<const char*>(metadata.payload.data()), metadata.payload.size())).object();
        require(object["path"] == "nested/file.bin" && object["size"].toInteger() == contents.size(), "Wrong metadata");
        std::string received;
        for (int chunkIndex = 0; chunkIndex < 2; ++chunkIndex) {
            const auto chunk = readFrame(receiver);
            require(chunk.type == ConnectionSession::MessageType::FileChunk && chunk.requestId == 42,
                    "Wrong chunk frame");
            std::uint64_t offset = 0;
            for (int i = 0; i < 8; ++i) offset = (offset << 8) | chunk.payload.at(i);
            require(offset == received.size(), "Wrong chunk offset");
            received.append(reinterpret_cast<const char*>(chunk.payload.data() + 8), chunk.payload.size() - 8);
        }
        require(received == contents, "Binary data changed in transit");
        require(readFrame(receiver).type == ConnectionSession::MessageType::FileTransferComplete,
                "Missing completion frame");
        require(transfer.state() == FileTransfer::State::Completed, "Transfer did not queue completion");
        // TODO: Move this removal to the completion frame's write callback.
        session->removeFileTransfer(42);
        auto& emptyTransfer = session->addFileTransfer(43,
            std::make_unique<FileTransfer>(session, shared, base / "downloads"));
        emptyTransfer.sendFile(43, "empty.bin"); flush();
        require(readFrame(receiver).type == ConnectionSession::MessageType::FileTransferStart, "Missing empty-file start");
        require(readFrame(receiver).type == ConnectionSession::MessageType::FileTransferComplete, "Empty file not completed");
        for (const auto& path : {std::filesystem::path("../outside.bin"), std::filesystem::path("nested"),
                                  std::filesystem::path("missing.bin"), shared / "empty.bin"}) {
            bool rejected = false;
            try { emptyTransfer.sendFile(44, path); } catch (const std::exception&) { rejected = true; }
            require(rejected && emptyTransfer.state() == FileTransfer::State::Failed, "Invalid path was accepted");
        }
        const auto downloads = base / "downloads";
        std::filesystem::create_directory(downloads);
        auto& incoming = session->addFileTransfer(90, std::make_unique<FileTransfer>(session, shared, downloads));
        const std::span<const std::uint8_t> bytes(
            reinterpret_cast<const std::uint8_t*>(contents.data()), contents.size());
        bool idleRejected = false;
        try { incoming.receiveChunk(90, 0, bytes.first(1)); }
        catch (const std::logic_error&) { idleRejected = true; }
        require(idleRejected, "Idle receiver must reject chunks");
        incoming.beginReceive(90, "nested/download.bin", contents.size());
        for (const auto& payload : {std::vector<std::uint8_t>{}, std::vector<std::uint8_t>(7),
                std::vector<std::uint8_t>(8), std::vector<std::uint8_t>(9 + FileTransfer::ChunkSize)}) {
            relay::handleMessage(*session, {ConnectionSession::MessageType::FileChunk, 90, payload}); flush();
            const auto error = readFrame(receiver);
            require(error.type == ConnectionSession::MessageType::FileTransferError && error.requestId == 90,
                    "Malformed chunk must return an error");
            require(incoming.bytesTransferred() == 0, "Malformed chunk wrote file data");
        }
        relay::handleMessage(*session, {ConnectionSession::MessageType::FileChunk, 999, std::vector<std::uint8_t>(9)});
        flush();
        require(readFrame(receiver).requestId == 999, "Unknown chunk ID must return an error");
        auto deliver = [&](std::uint64_t offset, std::span<const std::uint8_t> data) {
            std::vector<std::uint8_t> payload(8);
            for (std::size_t i = 0; i < 8; ++i) payload[i] = static_cast<std::uint8_t>(offset >> (56 - 8 * i));
            payload.insert(payload.end(), data.begin(), data.end());
            relay::handleMessage(*session, {ConnectionSession::MessageType::FileChunk, 90, std::move(payload)});
        };
        deliver(1, bytes.first(1)); flush();
        require(readFrame(receiver).type == ConnectionSession::MessageType::FileTransferError &&
                incoming.bytesTransferred() == 0, "Out-of-order chunk must not write data");
        auto rejectChunk = [&](std::uint64_t id, std::uint64_t offset,
                               std::span<const std::uint8_t> data) {
            bool rejected = false;
            try { incoming.receiveChunk(id, offset, data); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected && incoming.bytesTransferred() == 0, "Invalid chunk changed progress");
        };
        rejectChunk(91, 0, bytes.first(1));
        rejectChunk(90, 1, bytes.first(1));
        rejectChunk(90, 0, {});
        rejectChunk(90, 0, bytes);
        deliver(0, bytes.first(FileTransfer::ChunkSize));
        flush();
        require(receiver.available() == 0, "Receiving a chunk must not send an acknowledgment");
        bool oversized = false;
        try { incoming.receiveChunk(90, FileTransfer::ChunkSize, bytes.first(4)); }
        catch (const std::invalid_argument&) { oversized = true; }
        require(oversized && incoming.bytesTransferred() == FileTransfer::ChunkSize,
                "Chunk exceeded remaining file size");
        deliver(FileTransfer::ChunkSize, bytes.last(3));
        flush();
        require(receiver.available() == 0, "Final chunk must not send an acknowledgment");
        require(incoming.state() == FileTransfer::State::Receiving, "Must wait for completion message");
        std::ifstream downloaded(downloads / "nested" / "download.bin.part", std::ios::binary);
        const std::string saved((std::istreambuf_iterator<char>(downloaded)), {});
        require(saved == contents, "Received binary file contents differ");
        require(!std::filesystem::exists(downloads / "nested" / "download.bin"), "File finalized prematurely");
        downloaded.close();
        bool wrongCompletion = false;
        try { incoming.finishReceive(91); } catch (const std::invalid_argument&) { wrongCompletion = true; }
        require(wrongCompletion && incoming.state() == FileTransfer::State::Receiving, "Wrong completion ID changed transfer");
        incoming.finishReceive(90);
        require(incoming.state() == FileTransfer::State::Completed &&
                std::filesystem::exists(downloads / "nested/download.bin") &&
                !std::filesystem::exists(downloads / "nested/download.bin.part"), "Download was not finalized");
        incoming.cancel(); incoming.cancel();
        require(std::filesystem::exists(downloads / "nested/download.bin"), "Cancel deleted completed download");

        auto& completeHandlerTransfer = session->addFileTransfer(300,
            std::make_unique<FileTransfer>(session, shared, downloads));
        completeHandlerTransfer.beginReceive(300, "handler-empty.bin", 0);
        relay::handleMessage(*session, {ConnectionSession::MessageType::FileTransferComplete, 300, {1}});
        flush();
        require(readFrame(receiver).type == ConnectionSession::MessageType::FileTransferError &&
                session->findFileTransfer(300) != nullptr, "Malformed completion removed the transfer");
        relay::handleMessage(*session, {ConnectionSession::MessageType::FileTransferComplete, 300, {}});
        require(session->findFileTransfer(300) == nullptr && std::filesystem::exists(downloads / "handler-empty.bin"),
                "Completion handler did not finalize the file");
        relay::handleMessage(*session, {ConnectionSession::MessageType::FileTransferComplete, 301, {}});
        flush();
        require(readFrame(receiver).requestId == 301, "Unknown completion ID did not return an error");
        auto& premature = session->addFileTransfer(302,
            std::make_unique<FileTransfer>(session, shared, downloads));
        premature.beginReceive(302, "premature.bin", 5);
        relay::handleMessage(*session, {ConnectionSession::MessageType::FileTransferComplete, 302, {}});
        flush();
        require(readFrame(receiver).requestId == 302 && session->findFileTransfer(302) == nullptr &&
                !std::filesystem::exists(downloads / "premature.bin.part"), "Failed completion did not clean up");
        FileTransfer emptyIncoming(session, shared, downloads);
        emptyIncoming.beginReceive(93, "empty.bin", 0);
        emptyIncoming.finishReceive(93);
        require(std::filesystem::file_size(downloads / "empty.bin") == 0, "Empty download not finalized");

        FileTransfer incomplete(session, shared, downloads);
        incomplete.beginReceive(94, "incomplete.bin", 10);
        bool incompleteRejected = false;
        try { incomplete.finishReceive(94); } catch (const std::runtime_error&) { incompleteRejected = true; }
        require(incompleteRejected && incomplete.state() == FileTransfer::State::Failed, "Incomplete download finalized");
        incomplete.cancel(); incomplete.cancel(); flush();
        const auto cancelled = readFrame(receiver);
        require(cancelled.type == ConnectionSession::MessageType::FileTransferError && cancelled.requestId == 94,
                "Cancellation was not sent to the peer");
        require(!std::filesystem::exists(downloads / "incomplete.bin.part"), "Cancelled temporary file remains");

        std::ofstream(downloads / "occupied.bin.part") << "keep";
        FileTransfer occupied(session, shared, downloads);
        bool occupiedRejected = false;
        try { occupied.beginReceive(95, "occupied.bin", 1); }
        catch (const std::filesystem::filesystem_error&) { occupiedRejected = true; }
        occupied.cancel();
        require(occupiedRejected && std::filesystem::file_size(downloads / "occupied.bin.part") == 4,
                "Cancellation removed another transfer's file");

        FileTransfer collision(session, shared, downloads);
        collision.beginReceive(96, "collision.bin", 0);
        std::ofstream(downloads / "collision.bin") << "keep";
        bool collisionRejected = false;
        try { collision.finishReceive(96); } catch (const std::filesystem::filesystem_error&) { collisionRejected = true; }
        require(collisionRejected && std::filesystem::file_size(downloads / "collision.bin") == 4,
                "Finalization overwrote an existing file");
        collision.cancel(); flush();
        require(readFrame(receiver).requestId == 96, "Wrong collision cancellation ID");

        FileTransfer outgoingCancelled(session, shared, downloads);
        outgoingCancelled.sendFile(97, "nested/file.bin"); flush();
        readFrame(receiver); readFrame(receiver);
        outgoingCancelled.cancel(); outgoingCancelled.cancel(); flush();
        require(readFrame(receiver).type == ConnectionSession::MessageType::FileTransferError &&
                outgoingCancelled.state() == FileTransfer::State::Cancelled, "Outgoing cancellation failed");
        require(std::filesystem::file_size(shared / "nested/file.bin") == contents.size(), "Cancel modified source file");
        auto& peerCancelled = session->addFileTransfer(400,
            std::make_unique<FileTransfer>(session, shared, downloads));
        peerCancelled.beginReceive(400, "peer-cancelled.bin", 1);
        for (const std::string invalidError : {"not-json", "[]", "{}", "{\"error\":0}", "{\"error\":\"\"}"}) {
            relay::handleMessage(*session, {ConnectionSession::MessageType::FileTransferError, 400,
                                           {invalidError.begin(), invalidError.end()}});
            require(session->findFileTransfer(400) == &peerCancelled &&
                    peerCancelled.state() == FileTransfer::State::Receiving, "Malformed error cancelled transfer");
        }
        const std::string remoteError = "{\"error\":\"Source file unavailable\"}";
        for (const auto id : {400, 400, 999}) {
            relay::handleMessage(*session, {ConnectionSession::MessageType::FileTransferError,
                                           static_cast<std::uint64_t>(id), {remoteError.begin(), remoteError.end()}});
        }
        flush();
        require(session->findFileTransfer(400) == nullptr &&
                !std::filesystem::exists(downloads / "peer-cancelled.bin.part"), "Peer error did not clean up download");
        require(receiver.available() == 0, "Error handler must not send an error response");
        FileTransfer interrupted(session, shared, downloads);
        interrupted.beginReceive(92, "interrupted.bin", 1);
        session->close();
        bool receiveDisconnected = false;
        try { interrupted.receiveChunk(92, 0, bytes.first(1)); }
        catch (const std::runtime_error&) { receiveDisconnected = true; }
        require(receiveDisconnected && interrupted.state() == FileTransfer::State::Failed &&
                interrupted.bytesTransferred() == 0, "Disconnected receiver must fail without writing");
        interrupted.cancel(); interrupted.cancel();
        require(interrupted.state() == FileTransfer::State::Cancelled &&
                !std::filesystem::exists(downloads / "interrupted.bin.part"), "Disconnected cancellation failed");
        bool disconnected = false;
        try { emptyTransfer.sendFile(45, "empty.bin"); } catch (const std::runtime_error&) { disconnected = true; }
        require(disconnected, "Closed session must be rejected");
        std::cout << "File sending and receiving checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

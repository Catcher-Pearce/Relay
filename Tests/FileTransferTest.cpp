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
        flush();
        auto metadata = readFrame(receiver);
        require(metadata.type == ConnectionSession::MessageType::FileTransferStart && metadata.requestId == 42,
                "Missing transfer metadata");
        const auto object = QJsonDocument::fromJson(QByteArray(
            reinterpret_cast<const char*>(metadata.payload.data()), metadata.payload.size())).object();
        require(object["path"] == "nested/file.bin" && object["size"].toInteger() == contents.size(), "Wrong metadata");
        bool busy = false;
        try { transfer.sendFile(43, "empty.bin"); } catch (const std::logic_error&) { busy = true; }
        require(busy && transfer.state() == FileTransfer::State::Sending, "Active transfer must not be overwritten");
        std::string received;
        for (int chunkIndex = 0; chunkIndex < 2; ++chunkIndex) {
            const auto chunk = readFrame(receiver);
            if (chunkIndex == 0) {
        for (auto badAck : {ConnectionSession::Message{ConnectionSession::MessageType::FileChunkAck, 999, std::vector<std::uint8_t>(8)},
                            ConnectionSession::Message{ConnectionSession::MessageType::FileChunkAck, 42, {}},
                            ConnectionSession::Message{ConnectionSession::MessageType::FileChunkAck, 42, std::vector<std::uint8_t>(8)}}) {
            const auto id = badAck.requestId;
            relay::handleMessage(*session, std::move(badAck)); flush();
            require(readFrame(receiver).requestId == id, "Invalid acknowledgment must return an error");
            require(session->findFileTransfer(42) == &transfer && transfer.bytesTransferred() == FileTransfer::ChunkSize,
                    "Invalid acknowledgment advanced or removed the transfer");
        }
            }
            require(chunk.type == ConnectionSession::MessageType::FileChunk && chunk.requestId == 42,
                    "Wrong chunk frame");
            std::uint64_t offset = 0;
            for (int i = 0; i < 8; ++i) offset = (offset << 8) | chunk.payload.at(i);
            require(offset == received.size(), "Wrong chunk offset");
            received.append(reinterpret_cast<const char*>(chunk.payload.data() + 8), chunk.payload.size() - 8);
            std::vector<std::uint8_t> ackPayload(8);
            const auto nextOffset = static_cast<std::uint64_t>(received.size());
            for (std::size_t i = 0; i < 8; ++i) {
                ackPayload[i] = static_cast<std::uint8_t>(nextOffset >> (56 - 8 * i));
            }
            relay::handleMessage(*session, {ConnectionSession::MessageType::FileChunkAck, 42, std::move(ackPayload)});
            flush();
        }
        require(received == contents, "Binary data changed in transit");
        require(readFrame(receiver).type == ConnectionSession::MessageType::FileTransferComplete,
                "Missing completion frame");
        require(session->findFileTransfer(42) == nullptr, "Completed transfer must be removed");
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
        FileTransfer incoming(session, shared, downloads);
        const std::span<const std::uint8_t> bytes(
            reinterpret_cast<const std::uint8_t*>(contents.data()), contents.size());
        bool idleRejected = false;
        try { incoming.receiveChunk(90, 0, bytes.first(1)); }
        catch (const std::logic_error&) { idleRejected = true; }
        require(idleRejected, "Idle receiver must reject chunks");
        incoming.beginReceive(90, "nested/download.bin", contents.size());
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
        incoming.receiveChunk(90, 0, bytes.first(FileTransfer::ChunkSize));
        flush();
        auto ack = readFrame(receiver);
        require(ack.type == ConnectionSession::MessageType::FileChunkAck && ack.requestId == 90,
                "Missing chunk acknowledgement");
        auto ackOffset = [](const ConnectionSession::Message& message) {
            require(message.payload.size() == 8, "Ack must contain an offset");
            std::uint64_t value = 0;
            for (auto byte : message.payload) value = (value << 8) | byte;
            return value;
        };
        require(ackOffset(ack) == FileTransfer::ChunkSize, "Wrong acknowledged offset");
        bool oversized = false;
        try { incoming.receiveChunk(90, FileTransfer::ChunkSize, bytes.first(4)); }
        catch (const std::invalid_argument&) { oversized = true; }
        require(oversized && incoming.bytesTransferred() == FileTransfer::ChunkSize,
                "Chunk exceeded remaining file size");
        incoming.receiveChunk(90, FileTransfer::ChunkSize, bytes.last(3));
        flush();
        require(ackOffset(readFrame(receiver)) == contents.size(), "Wrong final acknowledgment");
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

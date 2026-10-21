#include "FileTransfer.h"
#include "Connection/ConnectionSession.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <stdexcept>
#include <limits>
#include <vector>
#include <system_error>
#include <utility>

FileTransfer::FileTransfer(std::weak_ptr<ConnectionSession> session,
                           std::filesystem::path sharedFolder,
                           std::filesystem::path downloadsFolder)
    : session_(std::move(session)), sharedFolder_(std::move(sharedFolder)),
      downloadsFolder_(std::move(downloadsFolder)) {}

void FileTransfer::sendFile(TransferId id, const std::filesystem::path& path) {
    if (state_ == State::Sending || state_ == State::Receiving || ownsTemporaryFile_) {
        throw std::logic_error("A transfer is already active");
    }
    try {
        if (path.empty() || path.is_absolute() || path.has_root_path()) {
            throw std::invalid_argument("A shared-folder-relative file path is required");
        }
        const auto base = std::filesystem::canonical(sharedFolder_);
        const auto resolvedPath = std::filesystem::canonical(base / path);
        const auto [baseEnd, pathIt] = std::mismatch(
            base.begin(), base.end(), resolvedPath.begin(), resolvedPath.end());
        if (baseEnd != base.end()) {
            throw std::filesystem::filesystem_error(
                "Requested path is outside the shared folder", resolvedPath,
                std::make_error_code(std::errc::permission_denied));
        }
        if (!std::filesystem::is_regular_file(resolvedPath)) {
            throw std::invalid_argument("Requested path must be a regular file");
        }
        auto session = session_.lock();
        if (!session || !session->isOpen()) {
            throw std::runtime_error("The transfer session is disconnected");
        }
        const auto size = std::filesystem::file_size(resolvedPath);
        if (size > static_cast<std::uintmax_t>(std::numeric_limits<qint64>::max())) {
            throw std::overflow_error("File size exceeds JSON integer range");
        }
        source_.close();
        source_.clear();
        source_.open(resolvedPath, std::ios::binary);
        if (!source_) throw std::runtime_error("Could not open the requested file");

        filePath_ = resolvedPath;
        transferId_ = id;
        totalBytes_ = size;
        bytesTransferred_ = 0;
        state_ = State::Sending;
        const auto relative = resolvedPath.lexically_relative(base).generic_u16string();
        const auto metadata = QJsonDocument(QJsonObject{
            {"path", QString::fromStdU16String(relative)},
            {"size", static_cast<qint64>(totalBytes_)}
        }).toJson(QJsonDocument::Compact);
        session->send({ConnectionSession::MessageType::FileTransferStart, id,
                       {metadata.begin(), metadata.end()}});
        sendNextChunk();
    } catch (...) {
        source_.close();
        state_ = State::Failed;
        throw;
    }
}

void FileTransfer::sendNextChunk() {
    if (state_ != State::Sending) {
        throw std::logic_error("No outgoing transfer is active");
    }
    try {
        auto session = session_.lock();
        if (!session || !session->isOpen()) {
            throw std::runtime_error("The transfer session is disconnected");
        }
        if (bytesTransferred_ == totalBytes_) {
            // TODO: Call after the final chunk write completes, or immediately for an empty file.
            session->send({ConnectionSession::MessageType::FileTransferComplete, transferId_, {}});
            source_.close();
            state_ = State::Completed;
            return;
        }
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(ChunkSize, totalBytes_ - bytesTransferred_));
        // Chunk payload: 8-byte big-endian offset followed by raw file bytes.
        // The frame's request ID is the transfer ID.
        std::vector<std::uint8_t> payload(8 + count);
        for (std::size_t i = 0; i < 8; ++i) {
            payload[i] = static_cast<std::uint8_t>(bytesTransferred_ >> (56 - 8 * i));
        }
        source_.read(reinterpret_cast<char*>(payload.data() + 8),
                     static_cast<std::streamsize>(count));
        if (source_.gcount() != static_cast<std::streamsize>(count) || source_.bad()) {
            throw std::runtime_error("Could not read the complete chunk; the file may have changed");
        }
        // TODO: Attach a write-completion callback. Capture a weak session plus the
        // transfer ID, look the transfer up again, verify it is still Sending, and
        // call sendNextChunk(). Do not capture a raw FileTransfer pointer.
        session->send({ConnectionSession::MessageType::FileChunk, transferId_, std::move(payload)}, [id = transferId_](ConnectionSession& session) {
    auto* transfer = session.findFileTransfer(id);
    if (transfer && transfer->state() == State::Sending) {
        transfer->sendNextChunk();
    }
});
        bytesTransferred_ += count;
    } catch (...) {
        source_.close();
        state_ = State::Failed;
        throw;
    }
}

void FileTransfer::beginReceive(TransferId id, const std::filesystem::path& relativePath, std::uint64_t expectedSize) {
    if (state_ == State::Sending || state_ == State::Receiving || ownsTemporaryFile_) {
        throw std::logic_error("A transfer is already active");
    }
    if (relativePath.empty() || relativePath.has_root_path() ||
        relativePath.filename().empty() || relativePath.filename() == ".") {
        throw std::invalid_argument("A downloads-folder-relative file path is required");
    }
    const auto base = std::filesystem::canonical(downloadsFolder_);
    auto candidate = base;
    for (const auto& component : relativePath) {
        if (component == "..") {
            throw std::invalid_argument("Download paths must not contain '..'");
        }
        candidate /= component;
        // Reject even dangling links, which weakly_canonical cannot resolve.
        std::error_code error;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error("Could not inspect download path", candidate, error);
        }
        if (std::filesystem::is_symlink(status)) {
            throw std::filesystem::filesystem_error(
                "Download paths must not traverse symbolic links", candidate,
                std::make_error_code(std::errc::permission_denied));
        }
    }
    // Unlike canonical, this also accepts a destination that does not exist yet.
    const auto resolvedPath = std::filesystem::weakly_canonical(candidate);
    const auto [baseEnd, pathIt] = std::mismatch(
        base.begin(), base.end(), resolvedPath.begin(), resolvedPath.end());
    if (baseEnd != base.end() || pathIt == resolvedPath.end()) {
        throw std::filesystem::filesystem_error(
            "Requested destination is outside the downloads folder", resolvedPath,
            std::make_error_code(std::errc::permission_denied));
    }

    filePath_ = resolvedPath;
    temporaryPath_ = resolvedPath;
    temporaryPath_ += ".part";

    const auto occupied = [](const std::filesystem::path& path) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error("Could not inspect download destination", path, error);
        }
        return std::filesystem::exists(status);
    };
    if (occupied(filePath_) || occupied(temporaryPath_)) {
        throw std::filesystem::filesystem_error(
            "Download destination already exists",
            filePath_,
            std::make_error_code(std::errc::file_exists));
        }

    std::filesystem::create_directories(filePath_.parent_path());

    destination_.close();
    destination_.clear();
    destination_.open(temporaryPath_, std::ios::binary);

    if (!destination_) {
        throw std::runtime_error("Could not open temporary download file");
    }

    ownsTemporaryFile_ = true;
    transferId_ = id;
    totalBytes_ = expectedSize;
    bytesTransferred_ = 0;
    state_ = State::Receiving;
}

void FileTransfer::receiveChunk(TransferId id, std::uint64_t offset,
                                std::span<const std::uint8_t> data) {
    if (state_ != State::Receiving) {
        throw std::logic_error("No incoming transfer is active");
    }
    if (id != transferId_) {
        throw std::invalid_argument("Transfer ID does not match the incoming transfer");
    }
    if (offset != bytesTransferred_) {
        throw std::invalid_argument("Chunk offset does not match the next expected byte");
    }
    if (data.empty() || data.size() > ChunkSize) {
        throw std::invalid_argument("Chunk must contain between 1 and ChunkSize bytes");
    }
    // Subtract rather than add so an oversized chunk cannot overflow the counter.
    if (bytesTransferred_ > totalBytes_ || data.size() > totalBytes_ - bytesTransferred_) {
        throw std::invalid_argument("Chunk exceeds the advertised file size");
    }
    try {
        auto session = session_.lock();
        if (!session || !session->isOpen()) {
            throw std::runtime_error("The transfer session is disconnected");
        }
        if (!destination_.is_open() || !destination_) {
            throw std::runtime_error("The temporary download file is not writable");
        }
        destination_.write(reinterpret_cast<const char*>(data.data()),
                           static_cast<std::streamsize>(data.size()));
        // Detect buffered write failures before updating progress.
        destination_.flush();
        if (!destination_) {
            throw std::runtime_error("Could not write the complete chunk to the download file");
        }
        bytesTransferred_ += data.size();
        // Stay Receiving until FileTransferComplete verifies and finalizes the file.
    } catch (...) {
        destination_.close();
        state_ = State::Failed;
        throw;
    }
}

void FileTransfer::finishReceive(TransferId id) {
    if (state_ != State::Receiving) {
        throw std::logic_error("No incoming transfer is active");
    }
    if (id != transferId_) {
        throw std::invalid_argument("Transfer ID does not match the incoming transfer");
    }
    try {
        if (bytesTransferred_ != totalBytes_) {
            throw std::runtime_error("Download completed before all expected bytes were received");
        }
        if (!ownsTemporaryFile_ || !destination_.is_open()) {
            throw std::runtime_error("The temporary download file is not open");
        }
        destination_.flush();
        if (!destination_) throw std::runtime_error("Could not flush the download file");
        destination_.close();
        if (destination_.fail()) throw std::runtime_error("Could not close the download file");
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(temporaryPath_)) ||
            std::filesystem::file_size(temporaryPath_) != totalBytes_) {
            throw std::runtime_error("Temporary download file does not match the expected size");
        }
        std::error_code error;
        const auto status = std::filesystem::symlink_status(filePath_, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error("Could not inspect final download path", filePath_, error);
        }
        if (std::filesystem::exists(status)) {
            throw std::filesystem::filesystem_error("Download destination already exists", filePath_,
                std::make_error_code(std::errc::file_exists));
        }
        std::filesystem::rename(temporaryPath_, filePath_);
        ownsTemporaryFile_ = false;
        temporaryPath_.clear();
        state_ = State::Completed;
    } catch (...) {
        if (destination_.is_open()) destination_.close();
        state_ = State::Failed;
        throw;
    }
}

void FileTransfer::cancel(bool notifyPeer) {
    if (state_ == State::Cancelled || state_ == State::Completed) return;
    const bool shouldNotify = notifyPeer &&
        (state_ == State::Sending || state_ == State::Receiving || ownsTemporaryFile_);
    if (source_.is_open()) source_.close();
    if (destination_.is_open()) destination_.close();
    source_.clear();
    destination_.clear();
    try {
        // A rejected beginReceive must never delete somebody else's existing .part file.
        if (ownsTemporaryFile_) {
            std::filesystem::remove(temporaryPath_);
            ownsTemporaryFile_ = false;
            temporaryPath_.clear();
        }
    } catch (...) {
        state_ = State::Failed;
        throw;
    }
    state_ = State::Cancelled;
    if (shouldNotify) {
        if (auto session = session_.lock(); session && session->isOpen()) {
            const auto error = QJsonDocument(QJsonObject{{"error", "Transfer cancelled"}})
                .toJson(QJsonDocument::Compact);
            session->send({ConnectionSession::MessageType::FileTransferError, transferId_,
                           {error.begin(), error.end()}});
        }
    }
}

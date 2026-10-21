#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>

class ConnectionSession;

/**
 * @brief Sends or receives one file through a ConnectionSession.
 *
 * The session normally owns this object in its transfer map, keyed by request ID.
 * The weak session reference avoids an ownership cycle. The caller must keep the
 * transfer alive for every operation and call all methods on the network thread.
 * File reads, writes, and flushes are synchronous on that thread.
 *
 * Sending: sendFile() queues metadata and the first chunk. Automatic advancement
 * from socket write completion is a TODO; call sendNextChunk() manually until
 * that callback is implemented. Receiving: beginReceive() opens a .part file,
 * receiveChunk() appends each chunk, and finishReceive() renames the completed file.
 * This class does not parse frames. There are no application-level chunk acknowledgments.
 *
 * Wire format (the frame request ID is always the transfer ID):
 * - FileTransferStart: UTF-8 JSON with relative "path" and byte "size" fields.
 * - FileChunk: 8-byte big-endian offset followed by up to ChunkSize raw bytes.
 * - FileTransferComplete: empty payload.
 * - Cancellation: FileTransferError with a UTF-8 JSON "error" field.
 *
 * cancel() explicitly cleans up an unfinished download. Destruction closes the
 * streams but does not remove a .part file or notify the peer. Before removing a
 * failed or active receiver from the session map, call cancel().
 * Path checks assume local filesystem entries are not changed concurrently;
 * validation and opening/renaming are separate filesystem operations.
 */
class FileTransfer {
public:
    /// Session-scoped request ID used to correlate metadata and chunks.
    using TransferId = std::uint64_t;
    enum class State {
        Idle,       ///< No transfer has started.
        Sending,    ///< Source is open; chunks await socket-write completion callbacks.
        Receiving,  ///< Temporary destination is open, including while awaiting completion.
        Completed,  ///< Sender queued completion, or receiver finalized its file.
        Cancelled,  ///< Explicit cleanup succeeded; completed files are preserved.
        Failed      ///< A send, write, finalization, or cleanup operation failed.
    };
    /// Maximum file-data bytes per chunk; excludes its 8-byte offset prefix.
    static constexpr std::size_t ChunkSize = 64 * 1024;

    /**
     * @brief Stores the session and folder paths without starting I/O.
     * @param session Session used to queue transfer messages; does not extend its lifetime.
     * @param sharedFolder Existing directory containing files available to send.
     * @param downloadsFolder Existing directory under which downloads are created.
     */
    FileTransfer(std::weak_ptr<ConnectionSession> session,
                 std::filesystem::path sharedFolder,
                 std::filesystem::path downloadsFolder);

    /**
     * @brief Opens a source file, sends metadata, and queues its first chunk.
     * @param id Request ID under which the session registered this transfer.
     * @param relativePath Nonempty relative path under sharedFolder.
     *
     * Resolves symlinks and rejects targets outside the shared folder, non-files,
     * closed sessions, and sizes exceeding Qt's signed 64-bit JSON integer range.
     * Empty files queue start and completion messages without a data chunk.
     * @throws std::logic_error If a transfer is active or temporary cleanup is pending.
     * @throws std::exception On invalid paths, filesystem, session, or read errors.
     * Failures after the active-transfer guard set Failed and close the source.
     */
    void sendFile(TransferId id, const std::filesystem::path& relativePath);
    /**
     * @brief Queues one chunk, or completion if all source bytes were already queued.
     * @pre State is Sending. The previous chunk's socket write has completed.
     *
     * TODO: Invoke from a per-frame write-completion callback, including after the
     * final chunk. Until wired, callers advance manually. Do not call in an unbounded
     * loop because send() queues bytes in memory. Completion means queued locally;
     * it does not confirm that the receiver has finalized its file.
     * @throws std::logic_error If no outgoing transfer is active.
     * @throws std::runtime_error On disconnected sessions or incomplete reads;
     * these errors close the source and set Failed.
     */
    void sendNextChunk();

    /**
     * @brief Validates a destination and opens its binary .part file.
     * @param id Incoming transfer ID, taken from the metadata frame's request ID.
     * @param relativePath File path relative to downloadsFolder.
     * @param expectedSize Advertised file size in bytes; zero allows an empty file.
     *
     * Rejects rooted paths, '..', symlink components, and occupied final or .part
     * paths. Creates missing parent directories and starts progress at zero.
     * @throws std::exception On active transfers, invalid paths, collisions, or I/O errors.
     * Receiving is set only after the temporary file opens successfully.
     */
    void beginReceive(TransferId id, const std::filesystem::path& relativePath,
                      std::uint64_t expectedSize);
    /**
     * @brief Appends a validated chunk and updates local progress.
     * @param id Must match the active incoming transfer ID.
     * @param offset Must equal bytesTransferred(); out-of-order/duplicate chunks are rejected.
     * @param data Raw file bytes, excluding the wire payload's 8-byte offset prefix.
     * Must contain 1 through ChunkSize bytes and fit within the remaining size.
     * The span is consumed synchronously and is not retained.
     *
     * Flushes after writing; a stream flush is not a guarantee of durable
     * disk storage. Remains Receiving after the last chunk until finishReceive().
     * @throws std::logic_error If no incoming transfer is active.
     * @throws std::invalid_argument On ID, offset, or size mismatch, without writing.
     * @throws std::runtime_error On session/write failure; closes the destination
     * and sets Failed. Partial data may remain and should be removed by cancel().
     */
    void receiveChunk(TransferId id, std::uint64_t offset,
                      std::span<const std::uint8_t> data);
    /**
     * @brief Finalizes a download after its FileTransferComplete message.
     * @param id Must match the active incoming transfer ID.
     *
     * Verifies progress and actual file size, flushes/closes the stream, checks
     * for a final-path collision, then renames .part and sets Completed.
     * No completion acknowledgment is sent by this method.
     * @throws std::logic_error If no incoming transfer is active.
     * @throws std::invalid_argument On an ID mismatch, leaving the transfer unchanged.
     * @throws std::exception On incomplete data or filesystem errors; sets Failed
     * and retains ownership of the temporary file for cancel() to clean up.
     */
    void finishReceive(TransferId id);
    /**
     * @brief Closes streams and removes only a temporary file owned by this transfer.
     * @param notifyPeer False for local cleanup when the caller sends its own error response.
     *
     * Sets Cancelled and queues FileTransferError when cancelling an active transfer
     * or owned temporary download, provided the session is still open. A disconnected
     * session does not prevent local cleanup. Cancelled and Completed are no-ops;
     * source files and completed downloads are never removed.
     * @throws std::filesystem::filesystem_error If temporary-file removal fails;
     * state becomes Failed and cleanup can be retried.
     */
    void cancel(bool notifyPeer = true);

    /// Current local lifecycle state; not a statement of remote completion.
    State state() const { return state_; }
    /// Last successfully started transfer's ID; initially zero, unchanged by cancellation.
    TransferId transferId() const { return transferId_; }
    /// Outgoing: file bytes queued. Incoming: file bytes successfully written and flushed.
    /// Excludes protocol headers and is retained after completion/cancellation.
    std::uint64_t bytesTransferred() const { return bytesTransferred_; }
    /// Source size when sending, advertised size when receiving; initially zero.
    std::uint64_t totalBytes() const { return totalBytes_; }

private:
    // Lock before sending so this object does not extend the session's lifetime.
    std::weak_ptr<ConnectionSession> session_;
    std::filesystem::path sharedFolder_;
    std::filesystem::path downloadsFolder_;
    std::filesystem::path filePath_;
    std::filesystem::path temporaryPath_;
    std::ifstream source_;
    std::ofstream destination_;
    bool ownsTemporaryFile_ = false;
    State state_ = State::Idle;
    TransferId transferId_ = 0;
    std::uint64_t bytesTransferred_ = 0;
    std::uint64_t totalBytes_ = 0;
};

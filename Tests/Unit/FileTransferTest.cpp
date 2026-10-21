#include "FileShare/FileTransfer.h"
#include "TestFiles.h"
#include <QtTest>
#include <array>
#include <vector>

class FileTransferTest : public QObject {
    Q_OBJECT
    std::unique_ptr<test::Files> files;
    std::unique_ptr<FileTransfer> transfer;
    std::filesystem::path shared, downloads;
private slots:
    void init() {
        files = std::make_unique<test::Files>();
        shared = files->root / "shared";
        downloads = files->root / "downloads";
        std::filesystem::create_directory(shared);
        std::filesystem::create_directory(downloads);
        transfer = std::make_unique<FileTransfer>(std::weak_ptr<ConnectionSession>{}, shared, downloads);
    }
    void cleanup() { transfer.reset(); files.reset(); }
    void startsIdle() {
        QCOMPARE(transfer->state(), FileTransfer::State::Idle);
        QCOMPARE(transfer->bytesTransferred(), std::uint64_t(0));
    }
    void receivingCreatesTemporaryFile() {
        transfer->beginReceive(7, "nested/file.bin", 20);
        QCOMPARE(transfer->state(), FileTransfer::State::Receiving);
        QCOMPARE(transfer->transferId(), std::uint64_t(7));
        QCOMPARE(transfer->totalBytes(), std::uint64_t(20));
        QVERIFY(std::filesystem::exists(downloads / "nested/file.bin.part"));
        QVERIFY(!std::filesystem::exists(downloads / "nested/file.bin"));
    }
    void unsafeDestination_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("empty") << "";
        QTest::newRow("parent-traversal") << "../outside";
        QTest::newRow("nested-traversal") << "nested/../outside";
        QTest::newRow("directory-only") << "nested/";
        QTest::newRow("dot") << ".";
    }
    void unsafeDestination() {
        QFETCH(QString, path);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
            transfer->beginReceive(1, std::filesystem::path(path.toStdU16String()), 1));
        QVERIFY(std::filesystem::is_empty(downloads));
    }
    void absoluteDestinationIsRejected() {
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, transfer->beginReceive(1, files->root / "outside", 1));
    }
    void symbolicLinkDestinationIsRejected() {
        std::error_code error;
        std::filesystem::create_directory_symlink(files->root, downloads / "link", error);
        if (error) QSKIP("Creating symlinks is not permitted on this system");
        QVERIFY_THROWS_EXCEPTION(std::filesystem::filesystem_error, transfer->beginReceive(1, "link/outside", 1));
        std::filesystem::create_symlink(files->root / "missing", downloads / "file.part", error);
        if (error) QSKIP("Creating symlinks is not permitted on this system");
        QVERIFY_THROWS_EXCEPTION(std::filesystem::filesystem_error, transfer->beginReceive(1, "file", 1));
    }
    void existingTemporaryFileIsPreserved() {
        test::write(downloads / "file.part", "keep");
        QVERIFY_THROWS_EXCEPTION(std::filesystem::filesystem_error, transfer->beginReceive(1, "file", 1));
        transfer->cancel(false);
        QCOMPARE(test::read(downloads / "file.part"), std::string("keep"));
    }
    void activeTransferCannotBeReplaced() {
        transfer->beginReceive(1, "file", 1);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, transfer->beginReceive(2, "other", 2));
        QCOMPARE(transfer->transferId(), std::uint64_t(1));
    }
    void invalidChunk_data() {
        QTest::addColumn<quint64>("id");
        QTest::addColumn<quint64>("offset");
        QTest::addColumn<int>("size");
        QTest::newRow("wrong-id") << quint64(2) << quint64(0) << 1;
        QTest::newRow("wrong-offset") << quint64(1) << quint64(1) << 1;
        QTest::newRow("empty") << quint64(1) << quint64(0) << 0;
        QTest::newRow("exceeds-total") << quint64(1) << quint64(0) << 3;
        QTest::newRow("exceeds-chunk-limit") << quint64(1) << quint64(0) << int(FileTransfer::ChunkSize + 1);
    }
    void invalidChunk() {
        QFETCH(quint64, id); QFETCH(quint64, offset); QFETCH(int, size);
        transfer->beginReceive(1, "file", 2);
        const std::vector<std::uint8_t> bytes(size);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, transfer->receiveChunk(id, offset, bytes));
        QCOMPARE(transfer->state(), FileTransfer::State::Receiving);
        QCOMPARE(transfer->bytesTransferred(), std::uint64_t(0));
        QCOMPARE(std::filesystem::file_size(downloads / "file.part"), std::uintmax_t(0));
    }
    void idleTransferRejectsChunksAndCompletion() {
        const std::array<std::uint8_t, 1> bytes{1};
        QVERIFY_THROWS_EXCEPTION(std::logic_error, transfer->receiveChunk(1, 0, bytes));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, transfer->finishReceive(1));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, transfer->sendNextChunk());
    }
    void disconnectedReceiverFailsWithoutWriting() {
        transfer->beginReceive(1, "file", 1);
        const std::array<std::uint8_t, 1> bytes{1};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, transfer->receiveChunk(1, 0, bytes));
        QCOMPARE(transfer->state(), FileTransfer::State::Failed);
        QCOMPARE(transfer->bytesTransferred(), std::uint64_t(0));
    }
    void wrongCompletionIdPreservesTransfer() {
        transfer->beginReceive(1, "file", 0);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, transfer->finishReceive(2));
        QCOMPARE(transfer->state(), FileTransfer::State::Receiving);
    }
    void prematureCompletionFailsAndCanBeCancelled() {
        transfer->beginReceive(1, "file", 10);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, transfer->finishReceive(1));
        QCOMPARE(transfer->state(), FileTransfer::State::Failed);
        transfer->cancel(false);
        QVERIFY(!std::filesystem::exists(downloads / "file.part"));
    }
    void emptyDownloadFinalizesAndSurvivesCancellation() {
        transfer->beginReceive(1, "file", 0);
        transfer->finishReceive(1);
        QCOMPARE(transfer->state(), FileTransfer::State::Completed);
        QCOMPARE(std::filesystem::file_size(downloads / "file"), std::uintmax_t(0));
        QVERIFY(!std::filesystem::exists(downloads / "file.part"));
        transfer->cancel(false);
        QVERIFY(std::filesystem::exists(downloads / "file"));
    }
    void finalizationDoesNotOverwriteExistingFile() {
        transfer->beginReceive(1, "file", 0);
        test::write(downloads / "file", "keep");
        QVERIFY_THROWS_EXCEPTION(std::filesystem::filesystem_error, transfer->finishReceive(1));
        QCOMPARE(test::read(downloads / "file"), std::string("keep"));
    }
    void cancellationIsIdempotent() {
        transfer->beginReceive(1, "file", 5);
        transfer->cancel(false);
        transfer->cancel(false);
        QCOMPARE(transfer->state(), FileTransfer::State::Cancelled);
        QVERIFY(!std::filesystem::exists(downloads / "file.part"));
    }
    void sourceOutsideSharedFolderIsRejected() {
        test::write(files->root / "outside", "outside");
        QVERIFY_THROWS_EXCEPTION(std::filesystem::filesystem_error, transfer->sendFile(1, "../outside"));
        QCOMPARE(transfer->state(), FileTransfer::State::Failed);
    }
};
QTEST_APPLESS_MAIN(FileTransferTest)
#include "FileTransferTest.moc"

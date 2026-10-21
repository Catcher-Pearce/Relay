#include "SocketFixture.h"
#include "Connection/MessageHandler.h"
#include <QJsonDocument>
#include <QJsonObject>

class MessageHandlerTest : public QObject {
    Q_OBJECT
    std::unique_ptr<test::SocketFixture> fixture;
    std::filesystem::path shared, downloads;
    FileTransfer& incoming(std::uint64_t id, std::uint64_t size = 3) {
        auto& transfer = fixture->session->addFileTransfer(id, std::make_unique<FileTransfer>(
            fixture->session, shared, downloads));
        transfer.beginReceive(id, "file", size);
        return transfer;
    }
    void dispatch(test::Type type, std::uint64_t id, std::vector<std::uint8_t> payload = {}) {
        relay::handleMessage(*fixture->session, {type, id, std::move(payload)});
    }
    static std::vector<std::uint8_t> text(const std::string& value) { return {value.begin(), value.end()}; }
    void expectError(std::uint64_t id) {
        const auto response = fixture->readFrame();
        QCOMPARE(response.type, test::Type::FileTransferError);
        QCOMPARE(response.requestId, id);
        const auto json = QJsonDocument::fromJson(QByteArray(
            reinterpret_cast<const char*>(response.payload.data()), response.payload.size()));
        QVERIFY(json.isObject());
        QVERIFY(json.object()["error"].isString());
    }
private slots:
    void init() {
        fixture = std::make_unique<test::SocketFixture>();
        shared = fixture->files.root / "shared";
        downloads = fixture->files.root / "downloads";
        std::filesystem::create_directory(shared);
        std::filesystem::create_directory(downloads);
    }
    void cleanup() { fixture.reset(); }
    void pingEchoesBinaryPayloadAndRequestId() {
        const std::vector<std::uint8_t> payload{0, 255, 1, 0};
        dispatch(test::Type::Ping, 0x0102030405060708ULL, payload);
        const auto response = fixture->readFrame();
        QCOMPARE(response.type, test::Type::Pong);
        QCOMPARE(response.requestId, std::uint64_t(0x0102030405060708ULL));
        QVERIFY(response.payload == payload);
    }
    void pongDoesNotReply() {
        dispatch(test::Type::Pong, 7);
        fixture->poll();
        QCOMPARE(fixture->remote.available(), std::size_t(0));
    }
    void invalidFileRequest_data() {
        QTest::addColumn<QByteArray>("payload");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("embedded-null") << QByteArray("a\0b", 3);
    }
    void invalidFileRequest() {
        QFETCH(QByteArray, payload);
        dispatch(test::Type::FileRequest, 7, {payload.begin(), payload.end()});
        expectError(7);
        QVERIFY(fixture->session->findFileTransfer(7) == nullptr);
    }
    void invalidMetadata_data() {
        QTest::addColumn<QByteArray>("payload");
        QTest::newRow("not-json") << QByteArray("broken");
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("missing-fields") << QByteArray("{}");
        QTest::newRow("empty-path") << QByteArray(R"({"path":"","size":1})");
        QTest::newRow("negative-size") << QByteArray(R"({"path":"file","size":-1})");
        QTest::newRow("fractional-size") << QByteArray(R"({"path":"file","size":1.5})");
        QTest::newRow("string-size") << QByteArray(R"({"path":"file","size":"1"})");
    }
    void invalidMetadata() {
        QFETCH(QByteArray, payload);
        dispatch(test::Type::FileTransferStart, 7, {payload.begin(), payload.end()});
        expectError(7);
        QVERIFY(fixture->session->findFileTransfer(7) == nullptr);
    }
    void duplicateStartPreservesExistingTransfer() {
        auto* original = &incoming(7);
        dispatch(test::Type::FileTransferStart, 7, text(R"({"path":"file","size":3})"));
        expectError(7);
        QVERIFY(fixture->session->findFileTransfer(7) == original);
    }
    void malformedChunk_data() {
        QTest::addColumn<int>("size");
        QTest::newRow("missing-offset") << 0;
        QTest::newRow("short-offset") << 7;
        QTest::newRow("no-file-data") << 8;
        QTest::newRow("too-large") << int(FileTransfer::ChunkSize + 9);
    }
    void malformedChunk() {
        QFETCH(int, size);
        auto& transfer = incoming(7);
        dispatch(test::Type::FileChunk, 7, std::vector<std::uint8_t>(size));
        expectError(7);
        QCOMPARE(transfer.bytesTransferred(), std::uint64_t(0));
    }
    void unknownChunkReturnsError() {
        dispatch(test::Type::FileChunk, 99, std::vector<std::uint8_t>(9));
        expectError(99);
    }
    void wrongChunkOffsetDoesNotWrite() {
        auto& transfer = incoming(7);
        std::vector<std::uint8_t> payload(9); payload[7] = 1;
        dispatch(test::Type::FileChunk, 7, payload);
        expectError(7);
        QCOMPARE(transfer.bytesTransferred(), std::uint64_t(0));
    }
    void chunkAndCompletionSaveBinaryDataWithoutAcknowledgments() {
        incoming(7);
        std::vector<std::uint8_t> payload(8);
        payload.insert(payload.end(), {0, 255, 1});
        dispatch(test::Type::FileChunk, 7, payload);
        fixture->poll();
        QCOMPARE(fixture->remote.available(), std::size_t(0));
        dispatch(test::Type::FileTransferComplete, 7);
        QVERIFY(fixture->session->findFileTransfer(7) == nullptr);
        QCOMPARE(test::read(downloads / "file"), std::string("\0\xff\1", 3));
        QVERIFY(!std::filesystem::exists(downloads / "file.part"));
    }
    void completionWithPayloadPreservesTransfer() {
        auto* original = &incoming(7, 0);
        dispatch(test::Type::FileTransferComplete, 7, {1});
        expectError(7);
        QVERIFY(fixture->session->findFileTransfer(7) == original);
    }
    void unknownCompletionReturnsError() {
        dispatch(test::Type::FileTransferComplete, 99);
        expectError(99);
    }
    void prematureCompletionRemovesTemporaryFile() {
        incoming(7);
        dispatch(test::Type::FileTransferComplete, 7);
        expectError(7);
        QVERIFY(fixture->session->findFileTransfer(7) == nullptr);
        QVERIFY(!std::filesystem::exists(downloads / "file.part"));
    }
    void peerErrorCancelsDownloadWithoutReply() {
        incoming(7);
        for (int i = 0; i < 2; ++i)
            dispatch(test::Type::FileTransferError, 7, text(R"({"error":"Cancelled by peer"})"));
        fixture->poll();
        QVERIFY(fixture->session->findFileTransfer(7) == nullptr);
        QVERIFY(!std::filesystem::exists(downloads / "file.part"));
        QCOMPARE(fixture->remote.available(), std::size_t(0));
    }
    void malformedErrorDoesNotCancelDownload() {
        auto* original = &incoming(7);
        dispatch(test::Type::FileTransferError, 7, text(R"({"error":123})"));
        fixture->poll();
        QVERIFY(fixture->session->findFileTransfer(7) == original);
        QCOMPARE(fixture->remote.available(), std::size_t(0));
    }
};
QTEST_APPLESS_MAIN(MessageHandlerTest)
#include "MessageHandlerTest.moc"

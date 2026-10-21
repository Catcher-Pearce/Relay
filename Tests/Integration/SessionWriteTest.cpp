#include "SocketFixture.h"
#include <QJsonDocument>
#include <QJsonObject>

class SessionWriteTest : public QObject {
    Q_OBJECT
    std::unique_ptr<test::SocketFixture> fixture;
private slots:
    void init() { fixture = std::make_unique<test::SocketFixture>(); }
    void cleanup() { fixture.reset(); }
    void callbacksRunOnceAndPreserveQueueOrder() {
        int calls = 0;
        fixture->session->send({test::Type::Ping, 1, {}}, [&](ConnectionSession& session) {
            ++calls;
            session.send({test::Type::Ping, 3, {}}, [&](ConnectionSession&) { ++calls; });
        });
        fixture->session->send({test::Type::Ping, 2, {}}, [&](ConnectionSession&) { ++calls; });
        QCOMPARE(calls, 0);
        for (std::uint64_t id = 1; id <= 3; ++id) QCOMPARE(fixture->readFrame().requestId, id);
        fixture->poll();
        QCOMPARE(calls, 3);
    }
    void closingFromCallbackDropsLaterCallbacks() {
        int dropped = 0;
        fixture->session->send({test::Type::Ping, 1, {}}, [](ConnectionSession& session) { session.close(); });
        fixture->session->send({test::Type::Ping, 2, {}}, [&](ConnectionSession&) { ++dropped; });
        QCOMPARE(fixture->readFrame().requestId, std::uint64_t(1));
        fixture->poll();
        QVERIFY(!fixture->session->isOpen());
        QCOMPARE(dropped, 0);
    }
    void throwingCallbackClosesSession() {
        fixture->session->send({test::Type::Ping, 1, {}}, [](ConnectionSession&) { throw std::runtime_error("test"); });
        fixture->readFrame(); fixture->poll();
        QVERIFY(!fixture->session->isOpen());
    }
    void callbackCannotAccessRemovedTransfer() {
        const auto shared = fixture->files.root / "shared";
        std::string contents(FileTransfer::ChunkSize + 3, 'x');
        test::write(shared / "file", contents);
        auto& transfer = fixture->session->addFileTransfer(7,
            std::make_unique<FileTransfer>(fixture->session, shared, fixture->files.root / "downloads"));
        transfer.sendFile(7, "file");
        transfer.cancel(false);
        fixture->session->removeFileTransfer(7);
        QCOMPARE(fixture->readFrame().type, test::Type::FileTransferStart);
        QCOMPARE(fixture->readFrame().type, test::Type::FileChunk);
        fixture->poll();
        QCOMPARE(fixture->remote.available(), std::size_t(0));
    }
    void sendsFileAutomatically_data() {
        QTest::addColumn<int>("size");
        QTest::newRow("empty") << 0;
        QTest::newRow("single-byte") << 1;
        QTest::newRow("exact-chunk") << int(FileTransfer::ChunkSize);
        QTest::newRow("multiple-chunks") << int(FileTransfer::ChunkSize * 2 + 3);
    }
    void sendsFileAutomatically() {
        QFETCH(int, size);
        std::string expected(size, '\0');
        for (int i = 0; i < size; ++i) expected[i] = static_cast<char>(i % 256);
        const auto shared = fixture->files.root / "shared";
        test::write(shared / "file.bin", expected);
        auto& transfer = fixture->session->addFileTransfer(7,
            std::make_unique<FileTransfer>(fixture->session, shared, fixture->files.root / "downloads"));
        transfer.sendFile(7, "file.bin");
        const auto start = fixture->readFrame();
        QCOMPARE(start.type, test::Type::FileTransferStart);
        const auto metadata = QJsonDocument::fromJson(QByteArray(
            reinterpret_cast<const char*>(start.payload.data()), start.payload.size())).object();
        QCOMPARE(metadata["size"].toInteger(), qint64(size));
        QCOMPARE(metadata["path"].toString(), QString("file.bin"));
        std::string received;
        while (received.size() < expected.size()) {
            const auto chunk = fixture->readFrame();
            QCOMPARE(chunk.type, test::Type::FileChunk);
            QCOMPARE(chunk.requestId, std::uint64_t(7));
            QVERIFY(chunk.payload.size() > 8 && chunk.payload.size() <= FileTransfer::ChunkSize + 8);
            std::uint64_t offset = 0;
            for (int i = 0; i < 8; ++i) offset = (offset << 8) | chunk.payload[i];
            QCOMPARE(offset, std::uint64_t(received.size()));
            received.append(reinterpret_cast<const char*>(chunk.payload.data() + 8), chunk.payload.size() - 8);
        }
        QCOMPARE(received, expected);
        const auto complete = fixture->readFrame();
        QCOMPARE(complete.type, test::Type::FileTransferComplete);
        QCOMPARE(complete.requestId, std::uint64_t(7));
        QVERIFY(complete.payload.empty());
        QCOMPARE(transfer.state(), FileTransfer::State::Completed);
    }
};
QTEST_APPLESS_MAIN(SessionWriteTest)
#include "SessionWriteTest.moc"

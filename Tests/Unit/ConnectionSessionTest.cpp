#include "Connection/ConnectionSession.h"
#include "TestFiles.h"
#include <boost/asio.hpp>
#include <QtTest>

class ConnectionSessionTest : public QObject {
    Q_OBJECT
    test::Files files;
    boost::asio::io_context context;
    std::shared_ptr<ConnectionSession> makeSession(std::function<void()> onClosed = {}) {
        return std::make_shared<ConnectionSession>(boost::asio::ip::tcp::socket(context),
            ConnectionSession::Direction::Incoming, 1, std::move(onClosed), nullptr);
    }
    std::unique_ptr<FileTransfer> makeTransfer() {
        return std::make_unique<FileTransfer>(std::weak_ptr<ConnectionSession>{}, files.root, files.root);
    }
private slots:
    void unknownTransferReturnsNull() { QVERIFY(makeSession()->findFileTransfer(7) == nullptr); }
    void mapOwnsTransferAndRemovalReleasesIt() {
        const auto session = makeSession();
        auto owned = makeTransfer();
        const auto* pointer = owned.get();
        auto& added = session->addFileTransfer(7, std::move(owned));
        QVERIFY(!owned);
        QVERIFY(&added == pointer);
        QVERIFY(session->findFileTransfer(7) == pointer);
        session->removeFileTransfer(7);
        QVERIFY(session->findFileTransfer(7) == nullptr);
        session->removeFileTransfer(7);
    }
    void duplicateIdPreservesOriginalTransfer() {
        const auto session = makeSession();
        auto* original = &session->addFileTransfer(7, makeTransfer());
        QVERIFY_THROWS_EXCEPTION(std::logic_error, session->addFileTransfer(7, makeTransfer()));
        QVERIFY(session->findFileTransfer(7) == original);
    }
    void nullTransferIsRejected() {
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, makeSession()->addFileTransfer(7, nullptr));
    }
    void closedSessionRejectsNewTransfers() {
        const auto session = makeSession();
        session->close();
        QVERIFY_THROWS_EXCEPTION(std::logic_error, session->addFileTransfer(7, makeTransfer()));
    }
    void repeatedCloseNotifiesOnce() {
        int calls = 0;
        const auto session = makeSession([&] { ++calls; });
        session->close(); session->close();
        QCOMPARE(calls, 1);
    }
};
QTEST_APPLESS_MAIN(ConnectionSessionTest)
#include "ConnectionSessionTest.moc"

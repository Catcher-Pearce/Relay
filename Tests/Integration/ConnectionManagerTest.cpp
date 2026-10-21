#include "Connection/ConnectionManager.h"
#include "TestFiles.h"
#include <QtTest>
#include <boost/asio.hpp>

class ConnectionManagerTest : public QObject {
    Q_OBJECT
    std::unique_ptr<test::Files> files;
    std::unique_ptr<test::WorkingDirectory> workingDirectory;
    static ConnectionManager::Snapshot state(ConnectionManager& manager) {
        auto future = manager.snapshot();
        if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
            throw std::runtime_error("Network worker did not respond");
        return future.get();
    }
    static bool connected(ConnectionManager& low, ConnectionManager& high, bool preferred = false) {
        const auto a = state(low), z = state(high);
        if (a.connections.size() != 1 || z.connections.size() != 1 || a.pendingCount || z.pendingCount) return false;
        if (preferred && (a.connections.front().direction != ConnectionSession::Direction::Incoming ||
                          z.connections.front().direction != ConnectionSession::Direction::Outgoing)) return false;
        return a.connections.front().localEndpoint == z.connections.front().remoteEndpoint &&
               a.connections.front().remoteEndpoint == z.connections.front().localEndpoint;
    }
private slots:
    void init() {
        files = std::make_unique<test::Files>();
        workingDirectory = std::make_unique<test::WorkingDirectory>(files->root);
    }
    void cleanup() { workingDirectory.reset(); files.reset(); }
    void keepsSoleNonpreferredConnection() {
        ConnectionManager low(0, "peer-a"), high(0, "peer-z");
        low.establishListener(false);
        const auto port = high.establishListener(false);
        low.sendConnectionRequest("127.0.0.1", port);
        QTRY_VERIFY_WITH_TIMEOUT(connected(low, high), 3000);
        QCOMPARE(state(low).connections.front().direction, ConnectionSession::Direction::Outgoing);
    }
    void simultaneousConnections_data() {
        QTest::addColumn<bool>("highFirst");
        QTest::newRow("lower-starts-first") << false;
        QTest::newRow("higher-starts-first") << true;
    }
    void simultaneousConnections() {
        QFETCH(bool, highFirst);
        ConnectionManager low(0, "peer-a"), high(0, "peer-z");
        const auto lowPort = low.establishListener(false), highPort = high.establishListener(false);
        if (highFirst) {
            high.sendConnectionRequest("127.0.0.1", lowPort);
            low.sendConnectionRequest("127.0.0.1", highPort);
        } else {
            low.sendConnectionRequest("127.0.0.1", highPort);
            high.sendConnectionRequest("127.0.0.1", lowPort);
        }
        QTRY_VERIFY_WITH_TIMEOUT(connected(low, high, true), 3000);
    }
    void preferredDuplicateReplacesNonpreferredConnection() {
        ConnectionManager low(0, "peer-a"), high(0, "peer-z");
        const auto lowPort = low.establishListener(false), highPort = high.establishListener(false);
        low.sendConnectionRequest("127.0.0.1", highPort);
        QTRY_VERIFY_WITH_TIMEOUT(connected(low, high), 3000);
        high.sendConnectionRequest("127.0.0.1", lowPort);
        QTRY_VERIFY_WITH_TIMEOUT(connected(low, high, true), 3000);
    }
    void duplicateDoesNotReplacePreferredConnection() {
        ConnectionManager low(0, "peer-a"), high(0, "peer-z");
        const auto lowPort = low.establishListener(false);
        high.establishListener(false);
        high.sendConnectionRequest("127.0.0.1", lowPort);
        QTRY_VERIFY_WITH_TIMEOUT(connected(low, high, true), 3000);
        const auto original = state(low).connections.front().remoteEndpoint;
        // A raw duplicate proves rejection by observing EOF, rather than assuming
        // an arbitrary sleep is long enough for connect/Hello/close to complete.
        boost::asio::io_context context;
        boost::asio::ip::tcp::socket duplicate(context);
        duplicate.connect({boost::asio::ip::make_address("127.0.0.1"), lowPort});
        const std::vector<std::uint8_t> hello{2, 0,0,0,0,0,0,0,1, 0,0,0,6, 'p','e','e','r','-','z'};
        boost::asio::write(duplicate, boost::asio::buffer(hello));
        duplicate.non_blocking(true);
        auto duplicateClosed = [&] {
            std::array<char, 256> buffer;
            boost::system::error_code error;
            duplicate.read_some(boost::asio::buffer(buffer), error);
            return error == boost::asio::error::eof || error == boost::asio::error::connection_reset;
        };
        QTRY_VERIFY_WITH_TIMEOUT(duplicateClosed(), 3000);
        QVERIFY(connected(low, high, true));
        QVERIFY(state(low).connections.front().remoteEndpoint == original);
    }
    void disconnectRemovesSession() {
        ConnectionManager low(0, "peer-a");
        auto high = std::make_unique<ConnectionManager>(0, "peer-z");
        const auto lowPort = low.establishListener(false);
        high->establishListener(false);
        high->sendConnectionRequest("127.0.0.1", lowPort);
        QTRY_VERIFY_WITH_TIMEOUT(connected(low, *high), 3000);
        high.reset();
        QTRY_VERIFY_WITH_TIMEOUT(state(low).connections.empty(), 3000);
        QCOMPARE(state(low).pendingCount, std::size_t(0));
    }
};
QTEST_APPLESS_MAIN(ConnectionManagerTest)
#include "ConnectionManagerTest.moc"

#include "Gui/MainWindow.h"
#include "Local/Protocol.h"
#include "TestFiles.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QTableWidget>
#include <QTcpSocket>
#include <QTreeWidget>
#include <QtEndian>
#include <QtTest>

class GuiCoreTest : public QObject {
    Q_OBJECT
    std::unique_ptr<test::Files> files;
    QProcess daemon;
    QLocalSocket control;
    QTcpSocket peer;
    QString endpoint;
    QJsonObject latest;
    QByteArray frames;
    std::uint64_t downloadId = 0;
    std::unique_ptr<MainWindow> window;
    const QByteArray content = QByteArray::fromHex("610062ff");

    void frame(quint8 type, std::uint64_t id, const QByteArray& payload = {}) {
        QByteArray bytes(13, '\0');
        bytes[0] = static_cast<char>(type);
        qToBigEndian<quint64>(id, bytes.data() + 1);
        qToBigEndian<quint32>(payload.size(), bytes.data() + 9);
        peer.write(bytes + payload);
        peer.flush();
    }
    void handleFrames() {
        frames += peer.readAll();
        while (frames.size() >= 13) {
            const auto size = qFromBigEndian<quint32>(frames.constData() + 9);
            if (frames.size() < 13 + size) return;
            const auto type = static_cast<quint8>(frames[0]);
            const auto id = qFromBigEndian<quint64>(frames.constData() + 1);
            const auto payload = frames.mid(13, size);
            frames.remove(0, 13 + size);
            if (type == 3) {
                frame(4, id, R"({"entries":[{"path":"docs","type":"directory"},{"path":"docs/hello.txt","type":"file","size":4},{"path":"empty.txt","type":"file","size":0},{"path":"link","type":"symlink"}]})");
            } else if (type == 5) {
                QCOMPARE(payload, QByteArray("docs/hello.txt"));
                downloadId = id;
                frame(7, id, R"({"path":"docs/hello.txt","size":4})");
            }
        }
    }
    QTableWidget* table(const QString& heading) {
        for (auto* candidate : window->findChildren<QTableWidget*>())
            if (candidate->horizontalHeaderItem(0)->text() == heading) return candidate;
        return nullptr;
    }
    void openWindow() {
        window = std::make_unique<MainWindow>(endpoint, false);
        window->show();
    }
    QString jobStatus() const {
        const auto jobs = latest["downloads"].toArray();
        return jobs.isEmpty() ? QString() : jobs.last().toObject()["status"].toString();
    }

private slots:
    void init() {
        files = std::make_unique<test::Files>();
        const auto root = QString::fromStdU16String(files->root.u16string());
        endpoint = root + "/control";
        latest = {};
        frames.clear();
        downloadId = 0;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("HOME", root);
        environment.insert("XDG_DATA_HOME", root + "/data");
        daemon.setProcessEnvironment(environment);
        daemon.setWorkingDirectory(root);
        test::write(files->root / "identity/deviceid.txt", "local-device");
        daemon.start(QString::fromUtf8(RELAY_DAEMON_PATH),
                     {"--socket", endpoint, "--data-dir", root + "/identity", "--port", "0", "--no-discovery"});
        QVERIFY(daemon.waitForStarted());
        connect(&control, &QLocalSocket::readyRead, this, [this] {
            while (control.canReadLine()) {
                const auto message = QJsonDocument::fromJson(control.readLine()).object();
                if (message["type"] == "state") latest = message;
            }
        });
        QElapsedTimer timeout;
        timeout.start();
        while (control.state() != QLocalSocket::ConnectedState && timeout.elapsed() < 5000) {
            control.abort();
            control.connectToServer(endpoint);
            if (!control.waitForConnected(50)) QTest::qWait(30);
        }
        QVERIFY2(control.state() == QLocalSocket::ConnectedState, daemon.readAllStandardError().constData());
        QTRY_VERIFY(latest.contains("port"));
        connect(&peer, &QTcpSocket::readyRead, this, [this] { handleFrames(); });
        peer.connectToHost("127.0.0.1", latest["port"].toInt());
        QVERIFY(peer.waitForConnected());
        frame(2, 1, "test-peer");
        QTRY_COMPARE(latest["peers"].toArray().size(), 1);
    }
    void cleanup() {
        window.reset();
        if (control.state() == QLocalSocket::ConnectedState) {
            relay::local::send(&control, {{"command", "shutdown"}});
            control.flush();
        }
        if (!daemon.waitForFinished(3000)) { daemon.kill(); daemon.waitForFinished(); }
        control.disconnect(this);
        peer.disconnect(this);
        control.abort();
        peer.abort();
        files.reset();
    }
    void downloadsContinueWithWindowClosedAndStateReturnsOnReopen() {
        openWindow();
        auto* peers = table("Device ID");
        QVERIFY(peers);
        QTRY_COMPARE(peers->rowCount(), 1);
        peers->selectRow(0);
        auto* tree = window->findChild<QTreeWidget*>();
        QVERIFY(tree);
        QTRY_COMPARE(tree->topLevelItemCount(), 3);
        const auto matches = tree->findItems("hello.txt", Qt::MatchExactly | Qt::MatchRecursive);
        QCOMPARE(matches.size(), 1);
        QCOMPARE(matches.first()->parent()->text(0), QString("docs"));
        auto* download = qobject_cast<QPushButton*>(tree->itemWidget(matches.first(), 2));
        QVERIFY(download);
        const auto link = tree->findItems("link", Qt::MatchExactly).first();
        QVERIFY(!tree->itemWidget(link, 2));
        download->click();
        QTRY_VERIFY(downloadId != 0);
        QTRY_COMPARE(jobStatus(), QString("Downloading"));
        control.abort();
        window->close();
        window.reset();
        QCOMPARE(daemon.state(), QProcess::Running);
        // Complete the binary download while no GUI exists.
        frame(8, downloadId, QByteArray(8, '\0') + content);
        frame(10, downloadId);
        // There are no local control clients while the transfer finishes.
        QTRY_VERIFY(std::filesystem::exists(files->root / "relay/relay-downloads/docs/hello.txt"));
        latest = {};
        control.connectToServer(endpoint);
        QVERIFY(control.waitForConnected(2000));
        QTRY_COMPARE(jobStatus(), QString("Completed"));
        QCOMPARE(test::read(files->root / "relay/relay-downloads/docs/hello.txt"), content.toStdString());
        QVERIFY(!std::filesystem::exists(files->root / "relay/relay-downloads/docs/hello.txt.part"));
        openWindow();
        auto* transfers = table("File");
        QVERIFY(transfers);
        QTRY_COMPARE(transfers->rowCount(), 1);
        QCOMPARE(transfers->item(0, 0)->text(), QString("docs/hello.txt"));
        QCOMPARE(transfers->item(0, 1)->text(), QString("Completed"));
        peers = table("Device ID");
        QTRY_COMPARE(peers->rowCount(), 1);
        peers->selectRow(0);
        tree = window->findChild<QTreeWidget*>();
        QTRY_COMPARE(tree->topLevelItemCount(), 3);
        auto* restored = qobject_cast<QPushButton*>(tree->itemWidget(
            tree->findItems("hello.txt", Qt::MatchExactly | Qt::MatchRecursive).first(), 2));
        QVERIFY(restored);
        QCOMPARE(restored->text(), QString("Downloaded"));
        QVERIFY(!restored->isEnabled());
        // Polling peer snapshots must preserve the selected peer and its tree.
        QTest::qWait(700);
        QCOMPARE(peers->selectionModel()->selectedRows().size(), 1);
        QCOMPARE(tree->topLevelItemCount(), 3);
    }
    void aSecondCoreDoesNotDisruptTheRunningCore() {
        QProcess duplicate;
        const auto root = QString::fromStdU16String(files->root.u16string());
        duplicate.setProcessEnvironment(daemon.processEnvironment());
        duplicate.start(QString::fromUtf8(RELAY_DAEMON_PATH),
                        {"--socket", endpoint, "--data-dir", root + "/identity", "--port", "0", "--no-discovery"});
        QVERIFY(duplicate.waitForFinished(5000));
        QCOMPARE(duplicate.exitCode(), 1);
        QVERIFY(duplicate.readAllStandardError().contains("already running"));
        QCOMPARE(daemon.state(), QProcess::Running);
        latest = {};
        relay::local::send(&control, {{"command", "state"}});
        QTRY_VERIFY(latest.contains("deviceId"));
        QCOMPARE(latest["deviceId"].toString(), QString("local-device"));
    }
};
QTEST_MAIN(GuiCoreTest)
#include "GuiCoreTest.moc"

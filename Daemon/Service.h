#pragma once

#include "Auth/DeviceIdentity.h"
#include "Connection/ConnectionManager.h"
#include "FileShare/FolderManager.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QLocalServer>
#include <QPointer>
#include <QTimer>
#include <map>

class Service final : public QObject {
public:
    Service(const QString& endpoint, unsigned short port, bool discovery);
    ~Service() override;

private:
    struct Request {
        QString peer;
        QString token;
        QString path; // Empty for a directory listing.
        QPointer<QLocalSocket> client;
        std::future<bool> sent;
        qint64 started;
    };
    void acceptClients();
    void command(QLocalSocket* client, const QJsonObject& message);
    void poll();
    void fileEvent(const QString& peer, const ConnectionSession::Message& message);
    void fail(std::uint64_t id, const QString& error);
    void broadcastState();
    QJsonObject state() const;
    void updateDownload(std::uint64_t id, const QString& status, const QString& error = {});

    // Destroy/join the worker before destroying data used by its callbacks.
    QLocalServer server_;
    QList<QLocalSocket*> clients_;
    QJsonArray peers_;
    QJsonArray downloads_;
    std::map<std::uint64_t, Request> requests_;
    std::future<ConnectionManager::Snapshot> snapshot_;
    QElapsedTimer clock_;
    QTimer timer_;
    unsigned short port_;
    DeviceIdentity identity_;
    FolderManager folders_;
    ConnectionManager manager_;
};

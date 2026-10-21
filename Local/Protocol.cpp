#include "Protocol.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QStandardPaths>

QString relay::local::serverName() {
    const auto user = QCryptographicHash::hash(QDir::homePath().toUtf8(),
                                              QCryptographicHash::Sha256).toHex().left(20);
    return QDir::tempPath() + "/relay-" + QString::fromLatin1(user);
}

QString relay::local::dataDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

bool relay::local::send(QLocalSocket* socket, const QJsonObject& message) {
    if (!socket || socket->state() != QLocalSocket::ConnectedState) return false;
    const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    // Disconnect slow clients rather than accumulate unbounded snapshots/listings.
    if (bytes.size() > MaxFrameSize || socket->bytesToWrite() > 4 * MaxFrameSize) {
        socket->abort();
        return false;
    }
    return socket->write(bytes) == bytes.size();
}

#pragma once

#include <QJsonObject>
#include <QLocalSocket>

namespace relay::local {
// Newline-delimited compact JSON over a per-user local socket, never a TCP port.
constexpr qsizetype MaxFrameSize = 2 * 1024 * 1024;
QString serverName();
QString dataDirectory();
bool send(QLocalSocket* socket, const QJsonObject& message);
}

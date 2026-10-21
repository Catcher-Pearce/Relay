#include "Service.h"
#include "Local/Protocol.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace {
QString pathString(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}
}

Service::Service(const QString& endpoint, unsigned short port, bool discovery)
    : port_(port), manager_(port, identity_.getDeviceId()) {
    // main owns the endpoint lock before identity loading or service construction.
    QLocalServer::removeServer(endpoint);
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_.listen(endpoint)) throw std::runtime_error(server_.errorString().toStdString());
    connect(&server_, &QLocalServer::newConnection, this, [this] { acceptClients(); });
    manager_.setFileEventHandler([this](const std::string& peer, const auto& message) {
        // The manager is the last member, so its worker is joined before QObject teardown.
        QMetaObject::invokeMethod(this, [this, peer = QString::fromStdString(peer), message] {
            fileEvent(peer, message);
        }, Qt::QueuedConnection);
    });
    port_ = manager_.establishListener(discovery);
    clock_.start();
    connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
    timer_.start(250);
    poll();
}

void Service::acceptClients() {
    while (auto* client = server_.nextPendingConnection()) {
        clients_.append(client);
        client->setReadBufferSize(relay::local::MaxFrameSize + 1);
        connect(client, &QLocalSocket::disconnected, this, [this, client] {
            clients_.removeAll(client);
            client->deleteLater(); // Downloads belong to this service, not the socket.
        });
        connect(client, &QLocalSocket::readyRead, this, [this, client] {
            while (client->canReadLine()) {
                const auto line = client->readLine(relay::local::MaxFrameSize + 1);
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(line, &error);
                if (!line.endsWith('\n') || error.error != QJsonParseError::NoError || !document.isObject()) {
                    client->abort();
                    return;
                }
                command(client, document.object());
            }
            if (client->bytesAvailable() >= relay::local::MaxFrameSize) client->abort();
        });
        relay::local::send(client, state());
    }
}

Service::~Service() {
    timer_.stop();
    for (auto* client : std::as_const(clients_)) {
        client->disconnect(this);
        client->abort();
    }
    server_.close();
}

QJsonObject Service::state() const {
    return {{"type", "state"}, {"deviceId", QString::fromStdString(identity_.getDeviceId())},
            {"deviceName", QString::fromStdString(identity_.getDeviceName())},
            {"sharedFolder", pathString(folders_.sharedFolder())},
            {"downloadsFolder", pathString(folders_.downloadsFolder())},
            {"dataDirectory", QDir::currentPath()}, {"port", port_},
            {"peers", peers_}, {"downloads", downloads_}};
}

void Service::broadcastState() {
    const auto message = state();
    const auto clients = clients_; // send() may disconnect and remove a slow client.
    for (auto* client : clients) relay::local::send(client, message);
}

void Service::command(QLocalSocket* client, const QJsonObject& message) {
    const auto type = message.value("command").toString();
    const auto token = message.value("token").toString();
    const auto replyError = [&](const QString& error) {
        relay::local::send(client, {{"type", "error"}, {"token", token}, {"error", error}});
    };
    if (type == "state") { relay::local::send(client, state()); return; }
    if (type == "shutdown") { QCoreApplication::quit(); return; }
    if (type == "setName") {
        try {
            identity_.setDeviceName(message.value("name").toString().toUtf8().toStdString());
            manager_.reloadDeviceName();
            relay::local::send(client, {{"type", "nameSaved"}, {"token", token}});
            broadcastState();
        } catch (const std::exception& error) { replyError(QString::fromUtf8(error.what())); }
        return;
    }
    if (type != "list" && type != "download") { replyError("Unknown command"); return; }
    if (requests_.size() >= 128) { replyError("Too many pending requests; try again later"); return; }
    const auto peer = message.value("peer").toString();
    if (std::none_of(peers_.begin(), peers_.end(), [&](const auto& value) {
            return value.toObject().value("id").toString() == peer;
        })) { replyError("Peer is no longer connected"); return; }
    const auto path = type == "download" ? message.value("path").toString() : QString();
    if (type == "download") {
        const auto parts = path.split('/');
        if (path.isEmpty() || path.startsWith('/') || path.contains('\\') || path.contains(':') ||
            path.contains(QChar::Null) || parts.contains("..") || parts.contains(".") || parts.contains("")) {
            replyError("Invalid relative file path"); return;
        }
        for (const auto& value : downloads_) {
            const auto job = value.toObject();
            if (job["path"] == path && job["status"] == "Downloading") {
                replyError("This destination is already being downloaded"); return;
            }
        }
    }
    std::uint64_t id;
    do { id = QRandomGenerator::global()->generate64(); } while (id <= 1 || requests_.contains(id));
    const auto bytes = path.toUtf8();
    auto sent = manager_.sendMessage(peer.toStdString(),
        {type == "list" ? ConnectionSession::MessageType::FileSystemRequest : ConnectionSession::MessageType::FileRequest,
         id, {bytes.begin(), bytes.end()}});
    requests_.emplace(id, Request{peer, token, path, client, std::move(sent), clock_.elapsed()});
    if (type == "download") {
        // Keep active jobs and the latest completed jobs for a reopened GUI.
        while (downloads_.size() >= 100) {
            int removable = -1;
            for (int i = 0; i < downloads_.size(); ++i)
                if (downloads_[i].toObject()["status"] != "Downloading") { removable = i; break; }
            if (removable < 0) break;
            downloads_.removeAt(removable);
        }
        downloads_.append(QJsonObject{{"id", QString::number(id)}, {"peer", peer},
                                     {"path", path}, {"status", "Downloading"}});
        broadcastState();
    }
}

void Service::updateDownload(std::uint64_t id, const QString& status, const QString& error) {
    for (int i = 0; i < downloads_.size(); ++i) {
        auto job = downloads_[i].toObject();
        if (job["id"].toString() != QString::number(id)) continue;
        job["status"] = status;
        job["error"] = error;
        downloads_[i] = job;
        break;
    }
    broadcastState();
}

void Service::fail(std::uint64_t id, const QString& error) {
    const auto request = requests_.find(id);
    if (request == requests_.end()) return;
    const auto& job = request->second;
    if (job.path.isEmpty()) {
        relay::local::send(job.client, {{"type", "error"}, {"token", job.token}, {"error", error}});
    } else {
        updateDownload(id, "Failed", error);
    }
    requests_.erase(request);
}

void Service::fileEvent(const QString& peer, const ConnectionSession::Message& message) {
    const auto found = requests_.find(message.requestId);
    if (found == requests_.end() || found->second.peer != peer) return;
    const auto& request = found->second;
    using Type = ConnectionSession::MessageType;
    const auto document = QJsonDocument::fromJson(QByteArray(
        reinterpret_cast<const char*>(message.payload.data()), static_cast<qsizetype>(message.payload.size())));
    if (message.type == Type::FileTransferError) {
        fail(message.requestId, document.object().value("error").toString("Peer reported a transfer error"));
    } else if (message.type == Type::FileSystemResponse && request.path.isEmpty()) {
        if (!document.isObject() || !document.object().value("entries").isArray()) {
            fail(message.requestId, "Peer sent an invalid file listing"); return;
        }
        relay::local::send(request.client, {{"type", "listing"}, {"token", request.token},
            {"peer", peer}, {"entries", document.object().value("entries")}});
        requests_.erase(found);
    } else if (message.type == Type::FileTransferComplete && !request.path.isEmpty()) {
        updateDownload(message.requestId, "Completed");
        requests_.erase(found);
    }
}

void Service::poll() {
    using namespace std::chrono_literals;
    if (snapshot_.valid() && snapshot_.wait_for(0s) == std::future_status::ready) {
        const auto snapshot = snapshot_.get();
        peers_ = {};
        for (const auto& peer : snapshot.connections) {
            std::ostringstream endpoint;
            endpoint << peer.remoteEndpoint;
            peers_.append(QJsonObject{{"id", QString::fromStdString(peer.peerId)},
                                      {"address", QString::fromStdString(endpoint.str())}});
        }
        QList<std::uint64_t> disconnected;
        for (const auto& [id, request] : requests_) {
            if (std::none_of(peers_.begin(), peers_.end(), [&](const auto& value) {
                    return value.toObject()["id"].toString() == request.peer;
                })) disconnected.append(id);
        }
        for (const auto id : disconnected) fail(id, "Peer disconnected");
        broadcastState();
    }
    if (!snapshot_.valid()) snapshot_ = manager_.snapshot();
    QList<std::pair<std::uint64_t, QString>> failures;
    for (auto& [id, request] : requests_) {
        if (request.sent.valid() && request.sent.wait_for(0s) == std::future_status::ready && !request.sent.get())
            failures.append({id, "Peer is no longer connected"});
        else if (request.path.isEmpty() && clock_.elapsed() - request.started > 10000)
            failures.append({id, "Timed out waiting for the peer's file listing"});
    }
    for (const auto& [id, error] : failures) fail(id, error);
}

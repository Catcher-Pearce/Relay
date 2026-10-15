#include "ConnectionManager.h"
#include "ConnectionSession.h"
#include <boost/asio/post.hpp>
#include <utility>

#include "../Auth/DeviceIdentity.h"

ConnectionManager::ConnectionManager(unsigned short tcpPort, std::string deviceId)
    : deviceId_(deviceId.empty() ? getOrCreateDeviceId() : std::move(deviceId)),
      tcpPort_(tcpPort), work_(boost::asio::make_work_guard(context_)),
      discovery_(context_, *this, deviceId_, tcpPort_),
      listener_(context_, *this), sender_(context_, *this),
      worker_([this] { context_.run(); }) {}

ConnectionManager::~ConnectionManager() {
    // First stop callbacks, then close sessions while the manager still exists.
    context_.stop();
    worker_.join();
    while (!pendingConnections_.empty()) {
        auto session = pendingConnections_.begin()->second;
        session->close();
    }
    while (!connections_.empty()) {
        auto session = connections_.begin()->second;
        session->close();
    }
}

unsigned short ConnectionManager::establishListener() {
    const auto port = listener_.establishListener(tcpPort_);

    boost::asio::post(context_, [this] {
        discovery_.start();
    });

    return port;
}

void ConnectionManager::sendConnectionRequest(const std::string& address, unsigned short port) {
    boost::asio::post(context_, [this, address, port] {
        sender_.sendConnectionRequest(address, port);
    });
}

std::shared_ptr<ConnectionSession>
ConnectionManager::addPendingConnection(boost::asio::ip::tcp::socket socket, ConnectionSession::Direction direction) {
    const auto connectionId = nextConnectionId_++;
    auto session = std::make_shared<ConnectionSession>(std::move(socket), direction, connectionId,
        [this, connectionId] { removeConnection(connectionId); },
        [this, connectionId](const std::string& peerId) { addConnection(connectionId, peerId); });
    pendingConnections_.emplace(connectionId, session);
    session->start(deviceId_);
    return session;
}

void ConnectionManager::addConnection(std::uint64_t connectionId, const std::string& peerId) {
    const auto pending = pendingConnections_.find(connectionId);
    if (pending == pendingConnections_.end()) {
        return;
    }

    auto session = pending->second;

    const auto& localId = deviceId_;
    const auto direction = session->direction();

    if ((peerId > localId &&
         direction == ConnectionSession::Direction::Outgoing) ||
        (peerId < localId &&
         direction == ConnectionSession::Direction::Incoming)) {
        session->close();
        return;
    }

    // Keep the existing connection if this peer is already registered.
    if (!connections_.emplace(peerId, session).second) {
        session->close();
        return;
    }
    pendingConnections_.erase(pending);
}

void ConnectionManager::removeConnection(std::uint64_t connectionId) {
    pendingConnections_.erase(connectionId);
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        if (it->second->connectionId() == connectionId) {
            connections_.erase(it);
            return;
        }
    }
}

std::future<ConnectionManager::Snapshot> ConnectionManager::snapshot() {
    auto result = std::make_shared<std::promise<Snapshot>>();
    auto future = result->get_future();
    boost::asio::post(context_, [this, result] {
        Snapshot state{pendingConnections_.size(), {}};
        for (const auto& [peerId, session] : connections_) {
            state.connections.push_back({peerId, session->direction(),
                                         session->localEndpoint(), session->remoteEndpoint()});
        }
        result->set_value(std::move(state));
    });
    return future;
}

std::future<bool> ConnectionManager::sendMessage(const std::string& peerId,
                                                ConnectionSession::Message message) {
    auto result = std::make_shared<std::promise<bool>>();
    auto future = result->get_future();
    boost::asio::post(context_, [this, peerId, message = std::move(message), result]() mutable {
        const auto peer = connections_.find(peerId);
        if (peer == connections_.end()) {
            result->set_value(false);
            return;
        }
        peer->second->send(std::move(message));
        result->set_value(true);
    });
    return future;
}

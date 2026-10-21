#include "ConnectionManager.h"
#include "ConnectionSession.h"
#include <boost/asio/post.hpp>
#include <utility>
#include <iostream>

#include "../Auth/DeviceIdentity.h"

ConnectionManager::ConnectionManager(unsigned short tcpPort, std::string deviceId)
    : identity_(std::move(deviceId)),
      tcpPort_(tcpPort), work_(boost::asio::make_work_guard(context_)),
      discovery_(context_, *this, identity_, tcpPort_),
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

unsigned short ConnectionManager::establishListener(bool enableDiscovery) {
    const auto port = listener_.establishListener(tcpPort_);

    if (enableDiscovery) {
        boost::asio::post(context_, [this] {
            discovery_.start();
        });
    }

    return port;
}

void ConnectionManager::sendConnectionRequest(const std::string& address, unsigned short port) {
    boost::asio::post(context_, [this, address, port] {
        sender_.sendConnectionRequest(address, port);
    });
}

bool ConnectionManager::hasConnection(const std::string& peerId) const {
    return connections_.contains(peerId);
}

std::shared_ptr<ConnectionSession>
ConnectionManager::addPendingConnection(boost::asio::ip::tcp::socket socket, ConnectionSession::Direction direction) {
    const auto connectionId = nextConnectionId_++;
    auto session = std::make_shared<ConnectionSession>(std::move(socket), direction, connectionId,
        [this, connectionId] { removeConnection(connectionId); },
        [this, connectionId](const std::string& peerId) { addConnection(connectionId, peerId); });
    session->onFileEvent = [this, weak = std::weak_ptr<ConnectionSession>(session)](const auto& message) {
        if (auto source = weak.lock(); source && fileEventHandler_)
            fileEventHandler_(source->peerId(), message);
    };
    pendingConnections_.emplace(connectionId, session);
    session->start(identity_.getDeviceId());
    return session;
}

void ConnectionManager::addConnection(std::uint64_t connectionId, const std::string& peerId) {
    const auto pending = pendingConnections_.find(connectionId);
    if (pending == pendingConnections_.end()) {
        return;
    }

    auto session = pending->second;

    const auto preferred = peerId > identity_.getDeviceId()
        ? ConnectionSession::Direction::Incoming
        : ConnectionSession::Direction::Outgoing;

    auto existing = connections_.find(peerId);

    if (existing == connections_.end()) {
        connections_.emplace(peerId, session);
    } else if (session->direction() == preferred &&
               existing->second->direction() != preferred) {
        auto oldSession = existing->second;
        existing->second = session;
        oldSession->close();
    } else {
        session->close();
        return;
    }
    pendingConnections_.erase(pending);
    peers_[peerId].connected = true;
}

void ConnectionManager::removeConnection(std::uint64_t connectionId) {
    pendingConnections_.erase(connectionId);
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        if (it->second->connectionId() == connectionId) {
            if (auto peer = peers_.find(it->first); peer != peers_.end()) {
                peer->second.connected = false;
            }
            connections_.erase(it);
            return;
        }
    }
}

void ConnectionManager::peerDiscovered(const std::string& peerId, const std::string deviceName, const std::string& senderIp, unsigned short tcpPort) {
    peers_.insert_or_assign(
        peerId, PeerInfo{.deviceName = deviceName, .address = senderIp, .tcpPort = tcpPort,
                         .lastSeen = std::chrono::steady_clock::now(),
                         .connected = hasConnection(peerId)});
}

void ConnectionManager::removeExpiredPeers() {
    const auto now = std::chrono::steady_clock::now();
    constexpr auto timeout = std::chrono::seconds(20);
    for (auto peer = peers_.begin(); peer != peers_.end();) {
        if (now - peer->second.lastSeen > timeout && !hasConnection(peer->first)) {
            peer = peers_.erase(peer);
        } else {
            ++peer;
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

void ConnectionManager::setFileEventHandler(FileEventHandler handler) {
    boost::asio::post(context_, [this, handler = std::move(handler)]() mutable {
        fileEventHandler_ = std::move(handler);
    });
}

void ConnectionManager::reloadDeviceName() {
    boost::asio::post(context_, [this] {
        try { identity_.reloadDeviceName(); }
        catch (const std::exception& error) {
            std::cerr << "Could not refresh discovery name: " << error.what() << std::endl;
        }
    });
}

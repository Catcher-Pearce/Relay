#include "ConnectionManager.h"
#include "ConnectionSession.h"
#include <boost/asio/post.hpp>
#include <utility>

ConnectionManager::ConnectionManager()
    : work_(boost::asio::make_work_guard(context_)),
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

void ConnectionManager::establishListener(unsigned short port) {
    // Called once from main before it can initiate any other listener operation.
    listener_.establishListener(port);
}

void ConnectionManager::sendConnectionRequest(const std::string& address, unsigned short port) {
    boost::asio::post(context_, [this, address, port] {
        sender_.sendConnectionRequest(address, port);
    });
}

std::shared_ptr<ConnectionSession>
ConnectionManager::addPendingConnection(boost::asio::ip::tcp::socket socket) {
    const auto connectionId = nextConnectionId_++;
    auto session = std::make_shared<ConnectionSession>(std::move(socket), connectionId,
        [this, connectionId] { removeConnection(connectionId); },
        [this, connectionId](const std::string& peerId) { addConnection(connectionId, peerId); });
    pendingConnections_.emplace(connectionId, session);
    session->start();
    return session;
}

void ConnectionManager::addConnection(std::uint64_t connectionId, const std::string& peerId) {
    const auto pending = pendingConnections_.find(connectionId);
    if (pending == pendingConnections_.end()) {
        return;
    }
    auto session = pending->second;
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

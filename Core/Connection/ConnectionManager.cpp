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
ConnectionManager::addConnection(boost::asio::ip::tcp::socket socket) {
    const auto id = nextConnectionId_++;
    auto session = std::make_shared<ConnectionSession>(std::move(socket), id,
        [this, id] { removeConnection(id); });
    connections_.emplace(id, session);
    session->start();
    return session;
}

void ConnectionManager::removeConnection(std::uint64_t connectionId) {
    connections_.erase(connectionId);
}

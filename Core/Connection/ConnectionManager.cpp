//
// Created by catcherpearce on 10/7/26.
//

#include "ConnectionManager.h"
#include "ConnectionSession.h"
#include "MessageHandler.h"
#include <iostream>
#include <utility>

ConnectionManager::ConnectionManager()
    : listener(io_context, connectionsByPeer), sender(io_context) {}

ConnectionManager::~ConnectionManager() {
    io_context.stop();
    if (worker.joinable()) {
        worker.join();
    }
}

void ConnectionManager::establishListener() {
    if (worker.joinable()) {
        return;
    }
    listener.establishListener();
    worker = std::thread([this] {
        io_context.run();
    });
}

void ConnectionManager::sendConnectionRequest() {
    sender.sendConnectionRequest();
}

bool ConnectionManager::registerConnection(const std::string& peerId,
                                           std::shared_ptr<ConnectionSession> session) {
    if (peerId.empty() || !session) {
        return false;
    }
    return connectionsByPeer.emplace(peerId, std::move(session)).second;
}

std::shared_ptr<ConnectionSession>
ConnectionManager::findConnection(const std::string& peerId) const {
    auto found = connectionsByPeer.find(peerId);
    return found == connectionsByPeer.end() ? nullptr : found->second;
}

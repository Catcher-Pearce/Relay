//
// Created by catcherpearce on 10/7/26.
//

#include "ConnectionManager.h"

ConnectionManager::ConnectionManager()
    : listener(io_context), sender(io_context) {}

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

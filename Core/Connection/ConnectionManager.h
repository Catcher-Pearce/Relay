//
// Created by catcherpearce on 10/7/26.
//

#ifndef RELAY_CONNECTIONMANAGER_H
#define RELAY_CONNECTIONMANAGER_H

#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>
#include <thread>
#include "ConnectionMap.h"
#include "ConnectionListener.h"
#include "ConnectionSender.h"

class ConnectionSession;

class ConnectionManager {
public:
    ConnectionManager();
    ~ConnectionManager();
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;
    void establishListener();
    void sendConnectionRequest();
    bool registerConnection(const std::string& peerId, std::shared_ptr<ConnectionSession> session);
    std::shared_ptr<ConnectionSession> findConnection(const std::string& peerId) const;

private:
    boost::asio::io_context io_context;
    // Construct the map before objects that hold references to it.
    ConnectionMap connectionsByPeer;
    ConnectionListener listener;
    ConnectionSender sender;
    std::thread worker;
};


#endif //RELAY_CONNECTIONMANAGER_H

//
// Created by catcherpearce on 10/7/26.
//

#ifndef RELAY_CONNECTIONMANAGER_H
#define RELAY_CONNECTIONMANAGER_H

#include <boost/asio/io_context.hpp>
#include <thread>
#include "ConnectionListener.h"
#include "ConnectionSender.h"

class ConnectionManager {
public:
    ConnectionManager();
    ~ConnectionManager();
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;
    void establishListener();
    void sendConnectionRequest();

private:
    boost::asio::io_context io_context;
    ConnectionListener listener;
    ConnectionSender sender;
    std::thread worker;
};


#endif //RELAY_CONNECTIONMANAGER_H

//
// Created by catcherpearce on 10/7/26.
//

#ifndef RELAY_CONNECTIONLISTENER_H
#define RELAY_CONNECTIONLISTENER_H

#include <boost/asio/ip/tcp.hpp>
#include "ConnectionMap.h"

class ConnectionListener {
public:
    ConnectionListener(boost::asio::io_context& context,
                       ConnectionMap& connections);

    void establishListener();

private:
    void acceptNextConnection();
    boost::asio::ip::tcp::acceptor acceptor;
    ConnectionMap& connections_;
};

#endif //RELAY_CONNECTIONLISTENER_H

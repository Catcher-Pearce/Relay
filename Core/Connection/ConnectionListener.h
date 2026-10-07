//
// Created by catcherpearce on 10/7/26.
//

#ifndef RELAY_CONNECTIONLISTENER_H
#define RELAY_CONNECTIONLISTENER_H

#include <boost/asio/ip/tcp.hpp>

class ConnectionListener {
public:
    explicit ConnectionListener(boost::asio::io_context& context);
    void establishListener();

private:
    void acceptNextConnection();
    boost::asio::ip::tcp::acceptor acceptor;
};


#endif //RELAY_CONNECTIONLISTENER_H

//
// Created by catcherpearce on 10/7/26.
//

#ifndef RELAY_CONNECTIONSENDER_H
#define RELAY_CONNECTIONSENDER_H

#include <boost/asio/io_context.hpp>


class ConnectionSender {
public:
    explicit ConnectionSender(boost::asio::io_context& context);
    void sendConnectionRequest();

private:
    boost::asio::io_context& io_context;
};


#endif //RELAY_CONNECTIONSENDER_H

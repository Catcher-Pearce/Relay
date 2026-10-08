#pragma once
#include <boost/asio/ip/tcp.hpp>

class ConnectionManager;

class ConnectionListener {
public:
    ConnectionListener(boost::asio::io_context& context, ConnectionManager& manager);
    void establishListener(unsigned short port);

private:
    void acceptNextConnection();
    boost::asio::ip::tcp::acceptor acceptor_;
    ConnectionManager& manager_;
};

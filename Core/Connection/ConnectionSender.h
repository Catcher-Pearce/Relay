#pragma once
#include <boost/asio/io_context.hpp>
#include <string>

class ConnectionManager;

class ConnectionSender {
public:
    ConnectionSender(boost::asio::io_context& context, ConnectionManager& manager);
    void sendConnectionRequest(const std::string& address, unsigned short port);

private:
    boost::asio::io_context& context_;
    ConnectionManager& manager_;
};

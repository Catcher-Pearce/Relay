#include "ConnectionListener.h"
#include "ConnectionManager.h"
#include <boost/asio.hpp>
#include <iostream>
#include <utility>

using boost::asio::ip::tcp;

ConnectionListener::ConnectionListener(boost::asio::io_context& context, ConnectionManager& manager)
    : acceptor_(context), manager_(manager) {}

void ConnectionListener::establishListener(unsigned short port) {
    tcp::endpoint endpoint(tcp::v4(), port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    std::cout << "Listening on port " << acceptor_.local_endpoint().port() << std::endl;
    acceptNextConnection();
}

void ConnectionListener::acceptNextConnection() {
    acceptor_.async_accept([this](boost::system::error_code error, tcp::socket socket) {
        if (error == boost::asio::error::operation_aborted) {
            return;
        }
        if (error) {
            std::cerr << "Accept error: " << error.message() << std::endl;
        } else {
            manager_.addConnection(std::move(socket));
        }
        acceptNextConnection();
    });
}

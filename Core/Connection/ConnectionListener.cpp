#include "ConnectionListener.h"
#include "ConnectionManager.h"
#include <boost/asio.hpp>
#include <iostream>
#include <utility>

using boost::asio::ip::tcp;

ConnectionListener::ConnectionListener(boost::asio::io_context& context, ConnectionManager& manager)
    : acceptor_(context), manager_(manager) {}

unsigned short ConnectionListener::establishListener(unsigned short port) {
    tcp::endpoint endpoint(tcp::v4(), port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    acceptNextConnection();
    return acceptor_.local_endpoint().port();
}

void ConnectionListener::acceptNextConnection() {
    acceptor_.async_accept([this](boost::system::error_code error, tcp::socket socket) {
        if (error == boost::asio::error::operation_aborted) {
            return;
        }
        if (error) {
            std::cerr << "Accept error: " << error.message() << std::endl;
        } else {
            manager_.addPendingConnection(std::move(socket), ConnectionSession::Direction::Incoming);
        }
        acceptNextConnection();
    });
}

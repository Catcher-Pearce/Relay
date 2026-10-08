#include "ConnectionListener.h"
#include "ConnectionSession.h"

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <string>

using boost::asio::ip::tcp;

#include <utility>

ConnectionListener::ConnectionListener(
    boost::asio::io_context& context,
    ConnectionMap& connections)
    : acceptor(context),
      connections_(connections)
{
}

void ConnectionListener::establishListener() {
    const tcp::endpoint endpoint(tcp::v4(), 12345);
    try {
        acceptor.open(endpoint.protocol());
        acceptor.set_option(tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();
        acceptNextConnection();
        std::cout << "Successfully listening on port " << endpoint.port()
                  << "..." << std::endl;
    } catch (...) {
        boost::system::error_code ignored;
        acceptor.close(ignored);
        throw;
    }
}

void ConnectionListener::acceptNextConnection() {
    acceptor.async_accept([this](boost::system::error_code error, tcp::socket socket) {
        if (error == boost::asio::error::operation_aborted) {
            return;
        }

        // Continue accepting while existing sessions receive messages.
        acceptNextConnection();
        if (error) {
            std::cerr << "Accept error: " << error.message() << std::endl;
            return;
        }

        auto session = std::make_shared<ConnectionSession>(std::move(socket), connections_);
        session->start();
    });
}

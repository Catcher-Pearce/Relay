#include "ConnectionListener.h"

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <string>

using boost::asio::ip::tcp;

ConnectionListener::ConnectionListener(boost::asio::io_context& context)
    : acceptor(context) {}

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

        // Keep accepting while this client's greeting is being sent.
        acceptNextConnection();
        if (error) {
            std::cerr << "Accept error: " << error.message() << std::endl;
            return;
        }

        auto connection = std::make_shared<tcp::socket>(std::move(socket));
        auto message = std::make_shared<std::string>("Hello from Relay!\n");

        boost::asio::async_write(*connection, boost::asio::buffer(*message),
            [connection, message](boost::system::error_code writeError, std::size_t) {
                if (writeError && writeError != boost::asio::error::operation_aborted) {
                    std::cerr << "Write error: " << writeError.message() << std::endl;
                }
                // Captures keep the socket and buffer alive through completion.
            });
    });
}

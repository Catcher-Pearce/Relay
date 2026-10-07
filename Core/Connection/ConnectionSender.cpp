//
// Created by catcherpearce on 10/7/26.
//

#include "ConnectionSender.h"

#include <boost/asio.hpp>
#include <iostream>
#include <string>

using boost::asio::ip::tcp;

ConnectionSender::ConnectionSender(boost::asio::io_context& context)
    : io_context(context) {}

void ConnectionSender::sendConnectionRequest() {
    try {
        boost::asio::ip::tcp::socket socket(io_context);

        std::string server_ip = "127.0.0.1";
        unsigned short port = 12345;

        boost::asio::ip::tcp::endpoint endpoint(
            boost::asio::ip::make_address(server_ip),
            port
        );

        std::cout << "Connecting to " << server_ip << ":" << port << "..." << std::endl;
        socket.connect(endpoint);

        std::cout << "Successfully connected!" << std::endl;

        socket.close();

    } catch (std::exception& e) {
        std::cerr << "Connection error: " << e.what() << std::endl;
    }
}


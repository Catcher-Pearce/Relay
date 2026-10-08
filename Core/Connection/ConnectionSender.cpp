#include "ConnectionSender.h"
#include "ConnectionManager.h"
#include "ConnectionSession.h"
#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <utility>

using boost::asio::ip::tcp;

ConnectionSender::ConnectionSender(boost::asio::io_context& context, ConnectionManager& manager)
    : context_(context), manager_(manager) {}

void ConnectionSender::sendConnectionRequest(const std::string& address, unsigned short port) {
    boost::system::error_code error;
    auto ip = boost::asio::ip::make_address(address, error);
    if (error) {
        std::cerr << "Invalid IP address: " << error.message() << std::endl;
        return;
    }
    auto socket = std::make_shared<tcp::socket>(context_);
    socket->async_connect(tcp::endpoint(ip, port),
        [this, socket](boost::system::error_code connectError) {
            if (connectError) {
                std::cerr << "Connect error: " << connectError.message() << std::endl;
                return;
            }
            auto session = manager_.addConnection(std::move(*socket));
            session->send({ConnectionSession::MessageType::Ping, 1, {}});
        });
}

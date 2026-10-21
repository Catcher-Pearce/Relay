//
// Created by catcherpearce on 10/15/26.
//

#ifndef RELAY_DISCOVERYMANAGER_H
#define RELAY_DISCOVERYMANAGER_H
#include <array>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/udp.hpp>

class ConnectionManager;
class DeviceIdentity;

class DiscoveryManager {

public:
    // Initializes resources on the shared network context. Does not start discovery.
    // connectionManager and identity must outlive this object and its asynchronous operations.
    // Read/update the identity on the shared network thread while discovery runs.
    DiscoveryManager(boost::asio::io_context& context,
                     ConnectionManager& connectionManager,
                     const DeviceIdentity& identity,
                     unsigned short tcpPort);

    void start();

    void broadcast();

    void scheduleBroadcast();

    void receive();

private:
    boost::asio::ip::udp::socket socket_;
    boost::asio::steady_timer timer_;

    std::array<char, 2048> receiveBuffer_;
    boost::asio::ip::udp::endpoint senderEndpoint_;
    boost::asio::ip::udp::endpoint broadcastEndpoint_;

    const DeviceIdentity& identity_;
    unsigned short tcpPort_;

    ConnectionManager& connectionManager_;
    bool stopped_ = true;
};


#endif //RELAY_DISCOVERYMANAGER_H

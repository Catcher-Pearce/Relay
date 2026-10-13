#pragma once
#include <boost/asio/io_context.hpp>
#include <string>

class ConnectionManager;

/**
 * @brief Opens outgoing TCP connections and registers successful sockets as pending.
 *
 * Operations run on the manager's network thread. The referenced context and
 * manager must outlive this sender and its outstanding connect callbacks.
 */
class ConnectionSender {
public:
    /**
     * @brief Stores the context and manager used for outgoing connections.
     * @param context Network context that executes asynchronous connect callbacks.
     * @param manager Owner that creates pending sessions after a successful connect.
     */
    ConnectionSender(boost::asio::io_context& context, ConnectionManager& manager);
    /**
     * @brief Starts an asynchronous connection to a numeric IP address.
     * @param address IPv4 or IPv6 address string; hostnames are not resolved.
     * @param port Remote TCP port.
     *
     * Invalid addresses and connection failures are logged. A successful connection
     * becomes a pending session, which sends Hello and waits for the remote peer ID.
     * @note Call on the network thread; ConnectionManager posts requests there.
     */
    void sendConnectionRequest(const std::string& address, unsigned short port);

private:
    boost::asio::io_context& context_;
    ConnectionManager& manager_;
};

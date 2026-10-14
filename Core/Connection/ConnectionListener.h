#pragma once
#include <boost/asio/ip/tcp.hpp>

class ConnectionManager;

/**
 * @brief Accepts incoming TCP sockets and gives them to the manager as pending sessions.
 *
 * The acceptor uses the manager's network context. Accept callbacks run on its
 * network thread. The manager must outlive this listener and its callbacks.
 */
class ConnectionListener {
public:
    /**
     * @brief Creates an acceptor without opening or binding it.
     * @param context Network context that executes asynchronous accept callbacks.
     * @param manager Owner that registers accepted sockets as pending connections.
     */
    ConnectionListener(boost::asio::io_context& context, ConnectionManager& manager);
    /**
     * @brief Opens an IPv4 listener and starts accepting connections.
     * @param port Local TCP port; zero lets the operating system choose a free port.
     * @return Bound local port.
     * @throws boost::system::system_error If opening, binding, or listening fails.
     * @note Call once during startup, before other listener operations can run.
     */
    unsigned short establishListener(unsigned short port);

private:
    /**
     * @brief Starts one asynchronous accept and schedules the next after completion.
     *
     * Successful accepts transfer their socket to the manager. Errors are logged
     * and accepting continues, except when the operation was aborted.
     */
    void acceptNextConnection();
    boost::asio::ip::tcp::acceptor acceptor_;
    ConnectionManager& manager_;
};

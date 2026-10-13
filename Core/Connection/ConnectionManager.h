#pragma once

#include "ConnectionListener.h"
#include "ConnectionSender.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

class ConnectionSession;

/**
 * @brief Owns the network worker, listener, sender, and connection registries.
 *
 * New sessions are stored by local numeric connection ID until Hello supplies
 * the remote device ID. They then move into the registry keyed by that peer ID.
 * Registry mutations run on the network thread, or after it stops during teardown.
 * The manager must outlive all operations that use its callbacks.
 */
class ConnectionManager {
public:
    /**
     * @brief Initializes networking and immediately starts the network worker.
     *
     * A work guard keeps the worker running even when there are no connections.
     */
    ConnectionManager();
    /**
     * @brief Stops and joins the worker, then closes pending and registered sessions.
     * @note Destroy from outside the network worker, with no concurrent manager calls.
     */
    ~ConnectionManager();
    /**
     * @brief Disables copying because the manager owns a worker and network resources.
     */
    ConnectionManager(const ConnectionManager&) = delete;
    /**
     * @brief Disables copy assignment to preserve unique network ownership.
     */
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    /**
     * @brief Starts accepting incoming IPv4 connections.
     * @param port Local TCP port; defaults to 12345, and zero requests a free port.
     * @throws boost::system::system_error If listener setup fails.
     * @note Call once during startup before other listener operations can run.
     */
    void establishListener(unsigned short port = 12345);
    /**
     * @brief Posts an outgoing connection request to the network worker.
     * @param address Numeric IPv4 or IPv6 address of the remote device.
     * @param port Remote TCP port, defaulting to 12345.
     *
     * Returns without waiting for a connection or Hello exchange. Address and
     * connection failures are logged by the sender.
     */
    void sendConnectionRequest(const std::string& address, unsigned short port = 12345);

    /**
     * @brief Takes ownership of a connected socket and starts its Hello exchange.
     * @param socket Connected socket transferred into the new session.
     * @return Shared ownership of the session, also retained in the pending registry.
     *
     * Assigns a local connection ID and installs callbacks for peer registration and
     * removal on close. start() sends the local device ID and begins reading messages.
     * @note Call only on the network thread.
     */
    std::shared_ptr<ConnectionSession> addPendingConnection(boost::asio::ip::tcp::socket socket);
    /**
     * @brief Releases registry ownership of a closed session by its local ID.
     * @param connectionId Local numeric ID assigned when the session was created.
     *
     * Searches both pending and registered sessions; an unknown ID is ignored.
     * This method removes ownership but does not itself close a socket.
     * @note Called by the close callback on the network thread or during teardown.
     */
    void removeConnection(std::uint64_t connectionId);

private:
    /**
     * @brief Promotes a pending session after its Hello callback supplies a peer ID.
     * @param connectionId Local numeric ID used to find the pending session.
     * @param peerId Remote device ID extracted from the Hello payload.
     *
     * Unknown pending IDs are ignored. If the peer ID is already registered, the
     * new session is closed and the existing connection remains registered.
     * @note Call only on the network thread through the session's Hello callback.
     */
    void addConnection(std::uint64_t connectionId, const std::string& peerId);
    boost::asio::io_context context_;
    // Keeps run() waiting even before any connections exist.
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::unordered_map<std::string, std::shared_ptr<ConnectionSession>> connections_;
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionSession>> pendingConnections_;
    std::uint64_t nextConnectionId_ = 1;
    ConnectionListener listener_;
    ConnectionSender sender_;
    std::thread worker_;
};

#pragma once

#include "ConnectionListener.h"
#include "ConnectionSender.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>
#include <future>
#include <vector>
#include <string>
#include <thread>
#include <unordered_map>

#include "ConnectionSession.h"
#include "Discovery/DiscoveryManager.h"

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
     * @brief Loads the local identity, initializes networking, and starts the worker.
     * @param tcpPort Local TCP listening port, supplied by the application.
     * @param deviceId Explicit identity, or empty to load/create the persistent device ID.
     *
     * A work guard keeps the worker running even when there are no connections.
     */
    explicit ConnectionManager(unsigned short tcpPort, std::string deviceId = {});
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
     * Uses the port supplied to the constructor; zero requests a free port.
     * @return Bound local port, including the assigned port when zero was requested.
     * @throws boost::system::system_error If listener setup fails.
     * @note Call once during startup before other listener operations can run.
     */
    // Discovery can be disabled for isolated localhost tests.
    unsigned short establishListener(bool enableDiscovery = true);

    struct ConnectionInfo {
        std::string peerId;
        ConnectionSession::Direction direction;
        boost::asio::ip::tcp::endpoint localEndpoint;
        boost::asio::ip::tcp::endpoint remoteEndpoint;
    };
    struct Snapshot {
        std::size_t pendingCount;
        std::vector<ConnectionInfo> connections;
    };
    // Read registries on the network worker; keep the manager alive until completion.
    std::future<Snapshot> snapshot();
    // Queues a message on the network worker. False means the peer is disconnected.
    std::future<bool> sendMessage(const std::string& peerId, ConnectionSession::Message message);
    /**
     * @brief Posts an outgoing connection request to the network worker.
     * @param address Numeric IPv4 or IPv6 address of the remote device.
     * @param port Remote TCP listening port.
     *
     * Returns without waiting for a connection or Hello exchange. Address and
     * connection failures are logged by the sender.
     */
    void sendConnectionRequest(const std::string& address, unsigned short port);

    /**
     * @brief Takes ownership of a connected socket and starts its Hello exchange.
     * @param socket Connected socket transferred into the new session.
     * @param direction Whether this computer accepted or initiated the socket.
     * @return Shared ownership of the session, also retained in the pending registry.
     *
     * Assigns a local connection ID and installs callbacks for peer registration and
     * removal on close. start() sends the local device ID and begins reading messages.
     * @note Call only on the network thread.
     */
    std::shared_ptr<ConnectionSession> addPendingConnection(boost::asio::ip::tcp::socket socket, ConnectionSession::Direction direction);
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
     * The first identified connection is retained regardless of direction. For
     * duplicates, the larger device ID prefers outgoing and the smaller incoming.
     * A preferred new session replaces a nonpreferred existing session; other
     * duplicates are closed. Unknown pending IDs are ignored.
     * @note Call only on the network thread through the session's Hello callback.
     */
    void addConnection(std::uint64_t connectionId, const std::string& peerId);
    const std::string deviceId_;
    const unsigned short tcpPort_;
    boost::asio::io_context context_;
    // Keeps run() waiting even before any connections exist.
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::unordered_map<std::string, std::shared_ptr<ConnectionSession>> connections_;
    DiscoveryManager discovery_;
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionSession>> pendingConnections_;
    std::uint64_t nextConnectionId_ = 1;
    ConnectionListener listener_;
    ConnectionSender sender_;
    std::thread worker_;
};

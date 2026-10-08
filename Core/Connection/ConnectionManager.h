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

class ConnectionManager {
public:
    ConnectionManager();
    ~ConnectionManager();
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    void establishListener(unsigned short port = 12345);
    void sendConnectionRequest(const std::string& address, unsigned short port = 12345);

    // Called only on the network thread by listener, sender, or session.
    std::shared_ptr<ConnectionSession> addConnection(boost::asio::ip::tcp::socket socket);
    void removeConnection(std::uint64_t connectionId);

private:
    boost::asio::io_context context_;
    // Keeps run() waiting even before any connections exist.
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionSession>> connections_;
    std::uint64_t nextConnectionId_ = 1;
    ConnectionListener listener_;
    ConnectionSender sender_;
    std::thread worker_;
};

#include "Connection/ConnectionManager.h"
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <iomanip>

namespace {
unsigned short parsePort(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("Port must be a number between 1 and 65535");
    }
    const auto port = std::stoul(text);
    if (port == 0 || port > 65535) {
        throw std::invalid_argument("Port must be between 1 and 65535");
    }
    return static_cast<unsigned short>(port);
}

void commandLoop(ConnectionManager& manager) {
    const auto help = "Commands: ping [PEER_ID], files [PEER_ID], download \"PATH\" [PEER_ID], peers, help, quit\n";
    std::cout << help;
    std::uint64_t requestId = 1;
    std::string line;
    while (std::cout << "relay> " << std::flush, std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command, peerId, extra, path;
        input >> command;
        if (command.empty()) continue;
        if (command == "download") {
            if (!(input >> std::quoted(path)) || path.empty() || path.find('\0') != std::string::npos) {
                std::cout << "Usage: download \"PATH\" [PEER_ID]\n";
                continue;
            }
        }
        input >> peerId >> extra;
        if (!extra.empty() || (!peerId.empty() && command != "ping" && command != "files" && command != "download")) {
            std::cout << help;
            continue;
        }
        if (command == "quit" || command == "exit") return;
        if (command == "help") {
            std::cout << help;
            continue;
        }
        if (command != "ping" && command != "files" && command != "download" && command != "peers") {
            std::cout << "Unknown command. " << help;
            continue;
        }
        const auto state = manager.snapshot().get();
        if (command == "peers") {
            if (state.connections.empty()) std::cout << "No connected peers.\n";
            for (const auto& peer : state.connections) {
                std::cout << peer.peerId << " at " << peer.remoteEndpoint << '\n';
            }
            continue;
        }
        if (peerId.empty()) {
            if (state.connections.size() != 1) {
                std::cout << (state.connections.empty() ? "No connected peers.\n"
                                                       : "Specify a PEER_ID from the peers command.\n");
                continue;
            }
            peerId = state.connections.front().peerId;
        }
        const auto id = requestId++;
        const auto type = command == "ping" ? ConnectionSession::MessageType::Ping
                        : command == "download" ? ConnectionSession::MessageType::FileRequest
                                                : ConnectionSession::MessageType::FileSystemRequest;
        std::vector<std::uint8_t> payload;
        if (command == "download") payload.assign(path.begin(), path.end());
        if (manager.sendMessage(peerId, {type, id, std::move(payload)}).get()) {
            std::cout << "Sent " << command << ", request " << id << " to " << peerId << std::endl;
        } else {
            std::cout << "Peer is not connected: " << peerId << std::endl;
        }
    }
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2 || argc > 4) {
            std::cerr << "Usage: Relay PEER_IP [LOCAL_PORT [PEER_PORT]]\n";
            return 1;
        }
        const auto localPort = argc >= 3 ? parsePort(argv[2]) : 12345;
        const auto peerPort = argc == 4 ? parsePort(argv[3]) : localPort;
        ConnectionManager manager;
        manager.establishListener(localPort);
        manager.sendConnectionRequest(argv[1], peerPort);
        auto stop = std::async(std::launch::async, [&manager] { commandLoop(manager); });
        bool reported = false;
        while (stop.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
            const auto state = manager.snapshot().get();
            if (state.connections.empty() && state.pendingCount == 0) {
                // The other computer may start later. Retry until it is available.
                manager.sendConnectionRequest(argv[1], peerPort);
                reported = false;
            } else if (!state.connections.empty() && !reported) {
                std::cout << "Connected to peer " << state.connections.front().peerId << std::endl;
                reported = true;
            }
        }
        stop.get();
    } catch (const std::exception& error) {
        std::cerr << "Relay error: " << error.what() << std::endl;
        return 1;
    }
}

#include "Connection/ConnectionManager.h"
#include "Auth/DeviceIdentity.h"
#include "FileShare/FolderManager.h"
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <iomanip>
#include <QCoreApplication>

namespace {
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
    QCoreApplication application(argc, argv);
    try {
        constexpr unsigned short tcpPort = 12345;
        DeviceIdentity identity;
        FolderManager folders;
        ConnectionManager manager(tcpPort, identity.getDeviceId());
        manager.establishListener();
        commandLoop(manager);
    } catch (const std::exception& error) {
        std::cerr << "Relay error: " << error.what() << std::endl;
        return 1;
    }
}

#include "Connection/ConnectionManager.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

ConnectionManager::Snapshot snapshot(ConnectionManager& manager) {
    auto result = manager.snapshot();
    require(result.wait_for(2s) == std::future_status::ready, "Network worker did not respond");
    return result.get();
}

void verifyPair(const ConnectionManager::Snapshot& low, const ConnectionManager::Snapshot& high) {
    require(low.pendingCount == 0 && high.pendingCount == 0, "Pending connections were not removed");
    require(low.connections.size() == 1 && high.connections.size() == 1, "Each peer must retain exactly one connection");
    const auto& incoming = low.connections.front();
    const auto& outgoing = high.connections.front();
    require(incoming.peerId == "peer-z" && outgoing.peerId == "peer-a", "Wrong peer identity");
    require(incoming.direction == ConnectionSession::Direction::Incoming, "Smaller ID must keep incoming");
    require(outgoing.direction == ConnectionSession::Direction::Outgoing, "Larger ID must keep outgoing");
    require(incoming.localEndpoint == outgoing.remoteEndpoint &&
            incoming.remoteEndpoint == outgoing.localEndpoint, "Peers retained different TCP connections");
}

void runPair(bool highStartsFirst) {
    ConnectionManager low(0, "peer-a"), high(0, "peer-z");
    const auto lowPort = low.establishListener(false);
    const auto highPort = high.establishListener(false);
    auto connect = [&] {
        if (highStartsFirst) {
            high.sendConnectionRequest("127.0.0.1", lowPort);
            low.sendConnectionRequest("127.0.0.1", highPort);
        } else {
            low.sendConnectionRequest("127.0.0.1", highPort);
            high.sendConnectionRequest("127.0.0.1", lowPort);
        }
    };
    connect();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
        const auto a = snapshot(low), z = snapshot(high);
        if (a.connections.size() == 1 && z.connections.size() == 1 &&
            a.pendingCount == 0 && z.pendingCount == 0) {
            verifyPair(a, z);
            break;
        }
        require(std::chrono::steady_clock::now() < deadline, "Peers failed to converge to one connection");
        std::this_thread::sleep_for(10ms);
    }
    const auto original = snapshot(low).connections.front().remoteEndpoint;
    // Repeat requests to exercise duplicate rejection and its pending-entry cleanup.
    connect();
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(10ms);
        const auto a = snapshot(low), z = snapshot(high);
        require(a.connections.size() == 1 && z.connections.size() == 1, "Established connection was lost");
        require(a.connections.front().remoteEndpoint == original, "Duplicate replaced the established socket");
        if (i == 49) verifyPair(a, z);
    }
}

void verifyOneSidedConnection() {
    ConnectionManager low(0, "peer-a"), high(0, "peer-z");
    low.establishListener(false);
    const auto port = high.establishListener(false);
    // This direction is nonpreferred, but must survive when it is the sole connection.
    low.sendConnectionRequest("127.0.0.1", port);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (snapshot(low).connections.empty() || snapshot(high).connections.empty()) {
        require(std::chrono::steady_clock::now() < deadline, "Single connection was closed after Hello");
        std::this_thread::sleep_for(10ms);
    }
    std::this_thread::sleep_for(100ms);
    const auto a = snapshot(low), z = snapshot(high);
    require(a.connections.size() == 1 && z.connections.size() == 1 &&
            a.pendingCount == 0 && z.pendingCount == 0, "Single connection did not remain registered");
    require(a.connections.front().remoteEndpoint == z.connections.front().localEndpoint,
            "Peers did not retain the same connection");
}

int main() {
    try {
        verifyOneSidedConnection();
        for (int i = 0; i < 6; ++i) runPair(i % 2 == 0);
        std::cout << "Both peers retained the same single connection; pending registries are empty.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

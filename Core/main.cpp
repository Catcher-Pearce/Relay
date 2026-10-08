#include "Connection/ConnectionManager.h"
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[]) {
    try {
        ConnectionManager manager;
        if (argc == 1) {
            manager.establishListener();
        } else {
            const std::string mode = argv[1];
            if ((mode == "--listen" && argc == 3) ||
                (mode == "--connect" && argc == 4)) {
                const auto port = std::stoul(argv[argc - 1]);
                if (port > 65535) {
                    throw std::invalid_argument("Port must be between 0 and 65535");
                }
                if (mode == "--listen") {
                    manager.establishListener(static_cast<unsigned short>(port));
                } else {
                    manager.sendConnectionRequest(argv[2], static_cast<unsigned short>(port));
                }
            } else {
                std::cerr << "Usage: Relay [--listen PORT | --connect IP PORT]\n";
                return 1;
            }
        }
        std::cout << "Press Enter to stop." << std::endl;
        std::cin.get();
    } catch (const std::exception& error) {
        std::cerr << "Relay error: " << error.what() << std::endl;
        return 1;
    }
}

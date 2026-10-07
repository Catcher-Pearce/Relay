//
// Created by catcherpearce on 10/7/26.
//

#include "Connection/ConnectionManager.h"
#include <iostream>
#include <exception>

int main() {
    try {
        ConnectionManager manager;
        manager.establishListener();
        std::cout << "Relay is running. Press Enter to stop." << std::endl;
        std::cin.get();
    } catch (const std::exception& error) {
        std::cerr << "Relay error: " << error.what() << std::endl;
        return 1;
    }
}

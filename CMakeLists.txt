cmake_minimum_required(VERSION 3.20)
project(Relay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost 1.70 REQUIRED CONFIG)
find_package(Threads REQUIRED)

add_executable(Relay
        Core/main.cpp
        Core/Connection/ConnectionListener.cpp
        Core/Connection/ConnectionListener.h
        Core/Connection/ConnectionSender.cpp
        Core/Connection/ConnectionSender.h
        Core/Connection/ConnectionManager.cpp
        Core/Connection/ConnectionManager.h
        Core/Connection/ConnectionSession.cpp
        Core/Connection/ConnectionSession.h
        Core/Connection/MessageHandler.cpp
        Core/Connection/MessageHandler.h
)

target_link_libraries(Relay PRIVATE Boost::headers Threads::Threads)

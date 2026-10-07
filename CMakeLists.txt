cmake_minimum_required(VERSION 4.3)
project(Relay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Boost 1.70 REQUIRED CONFIG)
find_package(Threads REQUIRED)
qt_standard_project_setup()

qt_add_executable(Relay
        Core/main.cpp
        Core/Connection/ConnectionListener.cpp
        Core/Connection/ConnectionListener.h
        Core/Connection/ConnectionSender.cpp
        Core/Connection/ConnectionSender.h
        Core/Connection/ConnectionManager.cpp
        Core/Connection/ConnectionManager.h
)

target_link_libraries(Relay PRIVATE Qt6::Widgets Boost::headers Threads::Threads)

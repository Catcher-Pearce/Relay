cmake_minimum_required(VERSION 3.20)
project(Relay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost 1.70 REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core)

add_library(RelayCore
        Core/FileShare/FolderManager.cpp
        Core/FileShare/FolderManager.h
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
        Core/Auth/DeviceIdentity.cpp
        Core/Auth/DeviceIdentity.h
        Core/FileShare/FileTransfer.cpp
        Core/FileShare/FileTransfer.h
)

target_include_directories(RelayCore PUBLIC Core)
target_link_libraries(RelayCore PUBLIC Boost::headers Threads::Threads)
target_link_libraries(RelayCore PUBLIC Qt6::Core)

add_executable(Relay Core/main.cpp)
target_link_libraries(Relay PRIVATE RelayCore)

include(CTest)
if(BUILD_TESTING)
    add_executable(ConnectionManagerTest Tests/ConnectionManagerTest.cpp)
    target_link_libraries(ConnectionManagerTest PRIVATE RelayCore)
    add_test(NAME simultaneous_connections COMMAND ConnectionManagerTest)
    set_tests_properties(simultaneous_connections PROPERTIES TIMEOUT 30)
    add_executable(FolderManagerTest Tests/FolderManagerTest.cpp)
    target_link_libraries(FolderManagerTest PRIVATE RelayCore)
    add_test(NAME folder_listing COMMAND FolderManagerTest)
    add_executable(FileTransferTest Tests/FileTransferTest.cpp)
    target_link_libraries(FileTransferTest PRIVATE RelayCore)
    add_test(NAME file_transfer_send COMMAND FileTransferTest)
    set_tests_properties(file_transfer_send PROPERTIES TIMEOUT 15)
endif()

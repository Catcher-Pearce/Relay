cmake_minimum_required(VERSION 3.20)
project(Relay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost 1.70 REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto SSL)

add_library(RelayCore
        Core/Connection/Discovery/DiscoveryManager.cpp
        Core/Connection/Discovery/DiscoveryManager.h
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
target_link_libraries(RelayCore PUBLIC OpenSSL::Crypto OpenSSL::SSL)

add_executable(Relay Core/main.cpp)
target_link_libraries(Relay PRIVATE RelayCore)

add_library(RelayLocal Local/Protocol.cpp Local/Protocol.h)
target_include_directories(RelayLocal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(RelayLocal PUBLIC Qt6::Core Qt6::Network)

add_executable(RelayDaemon Daemon/main.cpp Daemon/Service.cpp Daemon/Service.h)
target_link_libraries(RelayDaemon PRIVATE RelayCore RelayLocal)

option(RELAY_BUILD_GUI "Build the Qt Widgets desktop application" ON)
if(RELAY_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    add_executable(RelayGui Gui/main.cpp Gui/MainWindow.cpp Gui/MainWindow.h)
    target_link_libraries(RelayGui PRIVATE RelayLocal Qt6::Widgets)
    add_dependencies(RelayGui RelayDaemon)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    # Register each Qt Test method independently in CTest. Data rows retain their
    # individual names in Qt Test's output. Fixtures run afresh for every method.
    function(add_relay_test target source label)
        add_executable(${target} ${source})
        set_target_properties(${target} PROPERTIES AUTOMOC ON)
        target_include_directories(${target} PRIVATE Tests)
        target_link_libraries(${target} PRIVATE RelayCore Qt6::Test)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8)
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" declarations
             REGEX "^[ ]+void [A-Za-z0-9_]+\\(\\)")
        foreach(declaration IN LISTS declarations)
            string(REGEX MATCH "void ([A-Za-z0-9_]+)" match "${declaration}")
            set(method "${CMAKE_MATCH_1}")
            if(method STREQUAL "init" OR method STREQUAL "cleanup" OR method MATCHES "_data$")
                continue()
            endif()
            add_test(NAME "${label}.${target}.${method}" COMMAND ${target} ${method})
            set_tests_properties("${label}.${target}.${method}" PROPERTIES LABELS ${label} TIMEOUT 15)
        endforeach()
    endfunction()

    if(RELAY_BUILD_GUI)
        add_executable(GuiCoreTest Tests/Integration/GuiCoreTest.cpp Gui/MainWindow.cpp)
        set_target_properties(GuiCoreTest PROPERTIES AUTOMOC ON)
        target_include_directories(GuiCoreTest PRIVATE Tests)
        target_link_libraries(GuiCoreTest PRIVATE RelayLocal Qt6::Widgets Qt6::Test)
        target_compile_definitions(GuiCoreTest PRIVATE RELAY_DAEMON_PATH="$<TARGET_FILE:RelayDaemon>")
        add_dependencies(GuiCoreTest RelayDaemon)
        add_test(NAME integration.GuiCoreTest COMMAND GuiCoreTest)
        set_tests_properties(integration.GuiCoreTest PROPERTIES
            LABELS integration TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    add_relay_test(AuthenticatorTest Tests/Unit/AuthenticatorTest.cpp unit)
    add_relay_test(DeviceIdentityTest Tests/Unit/DeviceIdentityTest.cpp unit)
    add_relay_test(FolderManagerTest Tests/Unit/FolderManagerTest.cpp unit)
    add_relay_test(FileTransferTest Tests/Unit/FileTransferTest.cpp unit)
    add_relay_test(ConnectionSessionTest Tests/Unit/ConnectionSessionTest.cpp unit)
    add_relay_test(MessageHandlerTest Tests/Integration/MessageHandlerTest.cpp integration)
    add_relay_test(SessionWriteTest Tests/Integration/SessionWriteTest.cpp integration)
    add_relay_test(ConnectionManagerTest Tests/Integration/ConnectionManagerTest.cpp integration)
endif()

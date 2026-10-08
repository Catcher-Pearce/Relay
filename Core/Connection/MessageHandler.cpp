#include "MessageHandler.h"
#include <iostream>
#include <utility>

namespace relay {
void handleMessage(ConnectionSession& session, ConnectionSession::Message message) {
    if (message.type == ConnectionSession::MessageType::Ping) {
        std::cout << "Received PING, request " << message.requestId << std::endl;
        session.send({ConnectionSession::MessageType::Pong, message.requestId, std::move(message.payload)});
    } else {
        // A PONG is a response, so it does not trigger another response.
        std::cout << "Received PONG, request " << message.requestId << std::endl;
    }
}
}

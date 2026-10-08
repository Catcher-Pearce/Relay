#include "MessageHandler.h"

#include <iostream>

namespace relay {

void handleMessage(ConnectionMap& connections, ConnectionSession& session,
                   ConnectionSession::Message message) {
    std::cout << "Received message type " << static_cast<unsigned int>(message.type)
              << ", request " << message.requestId
              << ", payload " << message.payload.size() << " bytes\n";

    if (message.type == ConnectionSession::MessageType::Ping) {

    }
}

}

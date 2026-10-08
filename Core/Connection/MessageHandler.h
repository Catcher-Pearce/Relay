#ifndef RELAY_MESSAGEHANDLER_H
#define RELAY_MESSAGEHANDLER_H

#include "ConnectionSession.h"

namespace relay {

// Called on the network thread with a complete, decoded incoming message.
void handleMessage(ConnectionMap& connections, ConnectionSession& session, ConnectionSession::Message message);

}

#endif // RELAY_MESSAGEHANDLER_H

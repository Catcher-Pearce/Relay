#pragma once
#include "ConnectionSession.h"

namespace relay {
/**
 * @brief Dispatches a decoded message to its protocol action.
 * @param session Live session receiving the message.
 * @param message Decoded message; its payload can be moved into a response.
 *
 * Ping sends Pong with the same request ID and payload. Pong is logged without
 * replying. Hello decodes the payload as a peer ID and calls identifyPeer(), which
 * notifies the manager. Hello does not trigger another Hello response.
 * @note Called on the network thread after frame and greeting-order validation.
 */
void handleMessage(ConnectionSession& session, ConnectionSession::Message message);
}

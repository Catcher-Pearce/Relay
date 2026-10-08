#pragma once
#include "ConnectionSession.h"

namespace relay {
void handleMessage(ConnectionSession& session, ConnectionSession::Message message);
}

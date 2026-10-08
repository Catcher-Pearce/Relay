#ifndef RELAY_CONNECTIONMAP_H
#define RELAY_CONNECTIONMAP_H

#include <memory>
#include <string>
#include <unordered_map>

class ConnectionSession;

// Owned by ConnectionManager; access only on its network thread.
using ConnectionMap = std::unordered_map<std::string, std::shared_ptr<ConnectionSession>>;

#endif

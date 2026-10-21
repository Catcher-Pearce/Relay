//
// Created by catcherpearce on 10/8/26.
//

#ifndef RELAY_PEERID_H
#define RELAY_PEERID_H


#pragma once
#include <string>

/**
 * @brief Reads the persistent local device ID or creates and saves a random UUID.
 * @return Existing first whitespace-delimited token, or a newly generated UUID string.
 * @throws std::runtime_error If writing a newly generated ID fails.
 *
 * Uses deviceid.txt relative to the process working directory. If the file cannot
 * be read or contains no token, a new ID is generated and written to that path.
 * Existing tokens are returned without UUID validation. Calls access the file
 * directly and must be serialized to avoid racing ID creation.
 */
std::string getOrCreateDeviceId();


#endif //RELAY_PEERID_H

//
// Created by catcherpearce on 10/8/26.
//

#include "DeviceIdentity.h"
#include <filesystem>
#include <fstream>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

std::string getOrCreateDeviceId() {
    const auto path = std::filesystem::path("deviceid.txt");

    std::ifstream in(path);
    std::string id;

    if (in >> id) {
        return id;
    }

    id = boost::uuids::to_string(boost::uuids::random_generator()());

    std::ofstream out(path);

    if (!(out << id)) {
        throw std::runtime_error("could not save persistent id");
    }

    return id;
}

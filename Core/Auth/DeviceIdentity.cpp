//
// Created by catcherpearce on 10/8/26.
//

#include "DeviceIdentity.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

DeviceIdentity::DeviceIdentity(std::string deviceId)
    : deviceId_(deviceId.empty() ? getOrCreateDeviceId() : std::move(deviceId)),
      deviceName_(loadDeviceName()) {}

void DeviceIdentity::setDeviceName(std::string name) {
    std::ofstream out("devicename.txt", std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Could not open persistent device name for writing");
    out << name;
    out.close();
    if (!out) throw std::runtime_error("Could not save persistent device name");
    deviceName_ = std::move(name);
}

std::string DeviceIdentity::loadDeviceName() {
    const std::filesystem::path path("devicename.txt");
    if (!std::filesystem::exists(path)) return {};
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("Persistent device name is not a regular file");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Could not open persistent device name for reading");
    std::string name((std::istreambuf_iterator<char>(in)), {});
    if (in.bad()) throw std::runtime_error("Could not read persistent device name");
    return name;
}

std::string DeviceIdentity::getOrCreateDeviceId() {
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

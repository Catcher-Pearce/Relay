#pragma once

#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace test {
// Every fixture owns a fresh directory; tests never write to the user's Relay folders.
struct Files {
    QTemporaryDir temporary;
    std::filesystem::path root;
    Files() {
        if (!temporary.isValid()) throw std::runtime_error("Cannot create test directory");
        root = std::filesystem::path(temporary.path().toStdU16String());
    }
};
inline void write(const std::filesystem::path& path, const std::string& bytes = {}) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) throw std::runtime_error("Cannot write test fixture");
}
inline std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read test fixture");
    return {std::istreambuf_iterator<char>(stream), {}};
}
// DeviceIdentity uses relative files. Restore cwd before the temporary directory dies.
class WorkingDirectory {
    std::filesystem::path previous_ = std::filesystem::current_path();
public:
    explicit WorkingDirectory(const std::filesystem::path& path) { std::filesystem::current_path(path); }
    ~WorkingDirectory() { std::error_code error; std::filesystem::current_path(previous_, error); }
};
}

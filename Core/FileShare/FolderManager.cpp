//
// Created by catcherpearce on 10/13/26.
//

#include "FolderManager.h"

#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
std::filesystem::path defaultRelayFolder() {
    const auto home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (home.isEmpty()) {
        throw std::runtime_error("Could not locate the home folder");
    }

    return std::filesystem::path(home.toStdU16String()) / "relay";
}
}

FolderManager::FolderManager() : FolderManager(defaultRelayFolder()) {}

FolderManager::FolderManager(std::filesystem::path relayFolder)
    : sharedFolder_(relayFolder / "relay-shared"),
      downloadsFolder_(relayFolder / "relay-downloads") {
    std::filesystem::create_directories(sharedFolder_);
    std::filesystem::create_directories(downloadsFolder_);
}

QByteArray FolderManager::getFileSystem() const {
    std::vector<QJsonObject> entries;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(sharedFolder_)) {
        const auto relative = entry.path().lexically_relative(sharedFolder_).generic_u16string();
        QJsonObject item{{"path", QString::fromStdU16String(relative)}};
        const auto status = entry.symlink_status();
        if (std::filesystem::is_symlink(status)) {
            item.insert("type", "symlink");
        } else if (std::filesystem::is_directory(status)) {
            item.insert("type", "directory");
        } else if (std::filesystem::is_regular_file(status)) {
            item.insert("type", "file");
            const auto size = entry.file_size();
            if (size > static_cast<std::uintmax_t>(std::numeric_limits<qint64>::max())) {
                throw std::overflow_error("File size exceeds JSON integer range");
            }
            item.insert("size", static_cast<qint64>(size));
        } else {
            item.insert("type", "other");
        }
        entries.push_back(std::move(item));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.value("path").toString() < right.value("path").toString();
    });
    QJsonArray listing;
    for (const auto& entry : entries) listing.append(entry);
    return QJsonDocument(QJsonObject{{"entries", listing}}).toJson(QJsonDocument::Compact);
}

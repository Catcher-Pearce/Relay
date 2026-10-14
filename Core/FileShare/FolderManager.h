//
// Created by catcherpearce on 10/13/26.
//

#ifndef RELAY_FOLDERMANAGER_H
#define RELAY_FOLDERMANAGER_H

#include <filesystem>
#include <QByteArray>

class FolderManager {
public:
    // Creates Home/relay/relay-shared and Home/relay/relay-downloads if needed.
    // Throws on lookup or filesystem errors.
    FolderManager();
    // Creates both subdirectories under a supplied relay directory.
    explicit FolderManager(std::filesystem::path relayFolder);
    const std::filesystem::path& sharedFolder() const { return sharedFolder_; }
    const std::filesystem::path& downloadsFolder() const { return downloadsFolder_; }

    // UTF-8 JSON listing all entries recursively, without following symbolic links.
    // Paths are relative to the shared folder; file sizes are in bytes.
    // Throws on filesystem errors rather than returning an incomplete listing.
    QByteArray getFileSystem() const;

private:
    std::filesystem::path sharedFolder_;
    std::filesystem::path downloadsFolder_;
};


#endif //RELAY_FOLDERMANAGER_H

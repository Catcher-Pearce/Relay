#include "FileShare/FolderManager.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTemporaryDir>
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create temporary directory");
        const auto relayRoot = std::filesystem::path(temporary.path().toStdU16String()) / "relay";
        FolderManager manager(relayRoot);
        const auto root = manager.sharedFolder();
        require(root == relayRoot / "relay-shared", "Wrong shared directory");
        require(manager.downloadsFolder() == relayRoot / "relay-downloads", "Wrong downloads directory");
        require(std::filesystem::is_directory(root) &&
                std::filesystem::is_directory(manager.downloadsFolder()), "Both directories must be created");
        std::ofstream(manager.downloadsFolder() / "download.txt") << "download";
        FolderManager existing(relayRoot);
        require(std::filesystem::exists(existing.downloadsFolder() / "download.txt"),
                "Reopening folders must preserve downloads");
        require(QJsonDocument::fromJson(manager.getFileSystem()).object()["entries"].toArray().isEmpty(),
                "Empty folder should return an empty entries array");
        std::filesystem::create_directories(root / "nested" / "empty");
        std::ofstream(root / "nested" / "notes.txt", std::ios::binary) << "hello";
        std::ofstream(root / ".hidden", std::ios::binary);
        const auto unicodeName = std::filesystem::path(u"image \u00e9.txt");
        std::ofstream(root / unicodeName, std::ios::binary) << "abc";
        const auto external = std::filesystem::path(temporary.path().toStdU16String()) / "external";
        std::filesystem::create_directory(external);
        std::ofstream(external / "outside.txt") << "outside";
        std::error_code linkError;
        std::filesystem::create_directory_symlink(external, root / "link", linkError);

        QJsonParseError error;
        const auto json = manager.getFileSystem();
        const auto document = QJsonDocument::fromJson(json, &error);
        require(error.error == QJsonParseError::NoError && document.isObject(), "Listing must be valid JSON");
        const auto entries = document.object()["entries"].toArray();
        require(entries.size() == (linkError ? 5 : 6), "Unexpected number of entries");
        QJsonObject byPath;
        for (const auto& value : entries) {
            const auto item = value.toObject();
            byPath.insert(item["path"].toString(), item);
        }
        require(byPath["nested"].toObject()["type"] == "directory", "Missing directory");
        require(byPath["nested/empty"].toObject()["type"] == "directory", "Missing empty directory");
        require(byPath["nested/notes.txt"].toObject()["size"].toInteger() == 5, "Wrong file size");
        require(byPath.contains(".hidden"), "Missing hidden file");
        require(byPath.contains(QString::fromStdU16String(unicodeName.u16string())), "Missing Unicode filename");
        if (!linkError) require(byPath["link"].toObject()["type"] == "symlink", "Missing symbolic link");
        require(manager.getFileSystem() == json, "Listing should have stable ordering");
        std::filesystem::remove(root / "nested" / "notes.txt");
        require(QJsonDocument::fromJson(manager.getFileSystem()).object()["entries"].toArray().size() == entries.size() - 1,
                "Listing did not reflect file removal");
        std::filesystem::remove_all(root);
        bool threw = false;
        try { manager.getFileSystem(); } catch (const std::filesystem::filesystem_error&) { threw = true; }
        require(threw, "Missing root should report an error");
        std::cout << "Folder JSON listing checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

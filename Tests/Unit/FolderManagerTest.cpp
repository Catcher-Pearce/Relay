#include "FileShare/FolderManager.h"
#include "TestFiles.h"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

class FolderManagerTest : public QObject {
    Q_OBJECT
    std::unique_ptr<test::Files> files;
    std::unique_ptr<FolderManager> folders;
    QJsonObject listing() {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(folders->getFileSystem(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            throw std::runtime_error("Listing is not a JSON object");
        QJsonObject entries;
        for (const auto& value : document.object()["entries"].toArray()) {
            const auto item = value.toObject();
            entries.insert(item["path"].toString(), item);
        }
        return entries;
    }
private slots:
    void init() {
        files = std::make_unique<test::Files>();
        folders = std::make_unique<FolderManager>(files->root / "relay");
    }
    void cleanup() { folders.reset(); files.reset(); }
    void createsSharedAndDownloadDirectories() {
        QVERIFY(folders->sharedFolder() == files->root / "relay/relay-shared");
        QVERIFY(folders->downloadsFolder() == files->root / "relay/relay-downloads");
        QVERIFY(std::filesystem::is_directory(folders->sharedFolder()));
        QVERIFY(std::filesystem::is_directory(folders->downloadsFolder()));
    }
    void reopeningPreservesExistingFiles() {
        test::write(folders->downloadsFolder() / "keep.txt", "keep");
        FolderManager reopened(files->root / "relay");
        QCOMPARE(test::read(reopened.downloadsFolder() / "keep.txt"), std::string("keep"));
    }
    void emptyListingExcludesDownloads() {
        test::write(folders->downloadsFolder() / "private.txt", "private");
        QVERIFY(listing().isEmpty());
    }
    void listsNestedFilesAndEmptyDirectories() {
        test::write(folders->sharedFolder() / "nested/notes.txt", "hello");
        std::filesystem::create_directory(folders->sharedFolder() / "nested/empty");
        const auto entries = listing();
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries["nested"].toObject()["type"].toString(), QString("directory"));
        QCOMPARE(entries["nested/empty"].toObject()["type"].toString(), QString("directory"));
        QCOMPARE(entries["nested/notes.txt"].toObject()["size"].toInteger(), qint64(5));
    }
    void includesHiddenAndUnicodeFiles() {
        test::write(folders->sharedFolder() / ".hidden");
        test::write(folders->sharedFolder() / std::filesystem::path(u"image é.txt"), "abc");
        const auto entries = listing();
        QVERIFY(entries.contains(".hidden"));
        QCOMPARE(entries[QString::fromUtf8("image é.txt")].toObject()["size"].toInteger(), qint64(3));
    }
    void listsSymlinksWithoutFollowingThem() {
        test::write(files->root / "outside/private.txt", "outside");
        std::error_code error;
        std::filesystem::create_directory_symlink(files->root / "outside", folders->sharedFolder() / "link", error);
        if (error) QSKIP("Creating symlinks is not permitted on this system");
        const auto entries = listing();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries["link"].toObject()["type"].toString(), QString("symlink"));
    }
    void listingIsSortedAndReflectsChanges() {
        test::write(folders->sharedFolder() / "z");
        test::write(folders->sharedFolder() / "a");
        const auto json = folders->getFileSystem();
        QCOMPARE(folders->getFileSystem(), json);
        const auto entries = QJsonDocument::fromJson(json).object()["entries"].toArray();
        QCOMPARE(entries[0].toObject()["path"].toString(), QString("a"));
        std::filesystem::remove(folders->sharedFolder() / "a");
        QVERIFY(!listing().contains("a"));
    }
    void missingRootThrows() {
        std::filesystem::remove_all(folders->sharedFolder());
        QVERIFY_THROWS_EXCEPTION(std::filesystem::filesystem_error, folders->getFileSystem());
    }
};
QTEST_APPLESS_MAIN(FolderManagerTest)
#include "FolderManagerTest.moc"

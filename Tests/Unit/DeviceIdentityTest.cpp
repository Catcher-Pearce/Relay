#include "Auth/DeviceIdentity.h"
#include "TestFiles.h"
#include <QtTest>

class DeviceIdentityTest : public QObject {
    Q_OBJECT
    std::unique_ptr<test::Files> files;
    std::unique_ptr<test::WorkingDirectory> workingDirectory;
private slots:
    void init() {
        files = std::make_unique<test::Files>();
        workingDirectory = std::make_unique<test::WorkingDirectory>(files->root);
    }
    void cleanup() { workingDirectory.reset(); files.reset(); }
    void explicitIdDoesNotCreateIdFile() {
        DeviceIdentity identity("test-peer");
        QCOMPARE(identity.getDeviceId(), std::string("test-peer"));
        QVERIFY(!std::filesystem::exists("deviceid.txt"));
    }
    void generatedIdSurvivesReconstruction() {
        const DeviceIdentity first;
        QVERIFY(!first.getDeviceId().empty());
        QCOMPARE(DeviceIdentity().getDeviceId(), first.getDeviceId());
        QCOMPARE(test::read("deviceid.txt"), first.getDeviceId());
    }
    void existingIdIsPreserved() {
        test::write(files->root / "deviceid.txt", "existing-id\n");
        QCOMPARE(DeviceIdentity().getDeviceId(), std::string("existing-id"));
        QCOMPARE(test::read("deviceid.txt"), std::string("existing-id\n"));
    }
    void missingNameIsEmpty() { QVERIFY(DeviceIdentity("test-peer").getDeviceName().empty()); }
    void namesPersistWithSpacesAndUnicode() {
        DeviceIdentity identity("test-peer");
        const auto name = QString::fromUtf8("Office PC — é").toStdString();
        identity.setDeviceName(name);
        QCOMPARE(DeviceIdentity("test-peer").getDeviceName(), name);
        QCOMPARE(test::read("devicename.txt"), name);
        QCOMPARE(identity.getDeviceId(), std::string("test-peer"));
    }
    void emptyNameClearsPersistedName() {
        DeviceIdentity identity("test-peer");
        identity.setDeviceName("Old name");
        identity.setDeviceName("");
        QVERIFY(DeviceIdentity("test-peer").getDeviceName().empty());
    }
    void saveFailurePreservesCachedName() {
        DeviceIdentity identity("test-peer");
        identity.setDeviceName("Keep");
        std::filesystem::remove("devicename.txt");
        std::filesystem::create_directory("devicename.txt");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, identity.setDeviceName("Replacement"));
        QCOMPARE(identity.getDeviceName(), std::string("Keep"));
    }
    void unreadableNamePathReportsError() {
        std::filesystem::create_directory("devicename.txt");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, DeviceIdentity("test-peer"));
    }
};
QTEST_APPLESS_MAIN(DeviceIdentityTest)
#include "DeviceIdentityTest.moc"

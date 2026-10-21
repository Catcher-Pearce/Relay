#include "Service.h"
#include "Local/Protocol.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <iostream>

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName("Relay");
    QCommandLineParser parser;
    parser.setApplicationDescription("Relay background core. Closing a GUI does not stop this process.");
    parser.addHelpOption();
    parser.addOption({"background", "Start detached from the terminal."});
    parser.addOption({"stop", "Stop the running core at the selected socket endpoint."});
    parser.addOption({"socket", "Local control socket endpoint.", "path", relay::local::serverName()});
    parser.addOption({"data-dir", "Directory for device identity files.", "path", relay::local::dataDirectory()});
    parser.addOption({"port", "TCP listening port (0 chooses a free port).", "port", "12345"});
    parser.addOption({"no-discovery", "Disable LAN discovery for isolated tests."});
    parser.process(application);
    const auto endpoint = QDir::isAbsolutePath(parser.value("socket"))
        ? parser.value("socket") : QDir::temp().absoluteFilePath(parser.value("socket"));
    if (parser.isSet("stop")) {
        QLocalSocket socket;
        socket.connectToServer(endpoint);
        if (!socket.waitForConnected(2000)) {
            std::cerr << "Cannot connect to Relay core: " << socket.errorString().toStdString() << '\n';
            return 1;
        }
        if (!relay::local::send(&socket, {{"command", "shutdown"}}) ||
            (socket.bytesToWrite() && !socket.waitForBytesWritten(2000))) return 1;
        return 0;
    }
    bool valid = false;
    const auto port = parser.value("port").toUInt(&valid);
    if (!valid || port > 65535) { std::cerr << "Invalid TCP port\n"; return 1; }
    const auto dataDirectory = QFileInfo(parser.value("data-dir")).absoluteFilePath();
    if (!QDir().mkpath(dataDirectory)) { std::cerr << "Cannot create Relay data directory\n"; return 1; }
    if (parser.isSet("background")) {
        auto arguments = application.arguments().mid(1);
        arguments.removeAll("--background");
        QProcess process;
        process.setProgram(application.applicationFilePath());
        process.setArguments(arguments);
        process.setStandardOutputFile(dataDirectory + "/daemon.log", QIODevice::Append);
        process.setStandardErrorFile(dataDirectory + "/daemon.log", QIODevice::Append);
        return process.startDetached() ? 0 : 1;
    }
    try {
        // Lock before loading/creating identity files: simultaneous GUI launches
        // must not generate competing device IDs or remove a live control socket.
        QLockFile lock(endpoint + ".lock");
        if (!lock.tryLock(0)) throw std::runtime_error("Relay core is already running for this endpoint");
        // Preserve the existing identity on the first launch from an older Relay folder.
        if (!QFileInfo::exists(dataDirectory + "/deviceid.txt") && QFileInfo::exists("deviceid.txt")) {
            for (const auto& file : {QString("deviceid.txt"), QString("devicename.txt")}) {
                if (QFileInfo::exists(file) && !QFileInfo::exists(dataDirectory + "/" + file) &&
                    !QFile::copy(file, dataDirectory + "/" + file))
                    throw std::runtime_error("Could not migrate existing device identity");
            }
        }
        if (!QDir::setCurrent(dataDirectory)) throw std::runtime_error("Cannot open Relay data directory");
        Service service(endpoint, static_cast<unsigned short>(port), !parser.isSet("no-discovery"));
        return application.exec();
    } catch (const std::exception& error) {
        std::cerr << "Relay core: " << error.what() << '\n';
        return 1;
    }
}

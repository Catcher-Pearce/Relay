#include "MainWindow.h"
#include "Local/Protocol.h"
#include <QCommandLineParser>
#include <QApplication>
#include <QMessageBox>
#include <exception>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QApplication::setApplicationName("Relay");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"socket", "Background core socket endpoint.", "path", relay::local::serverName()});
    parser.addOption({"no-start-core", "Only attach to an existing core; do not start one."});
    parser.process(application);
    try {
        MainWindow window(parser.value("socket"), !parser.isSet("no-start-core"));
        window.show();
        return application.exec();
    } catch (const std::exception& error) {
        QMessageBox::critical(nullptr, "Relay could not start", QString::fromUtf8(error.what()));
        return 1;
    }
}

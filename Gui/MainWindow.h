#pragma once

#include <QJsonArray>
#include <QLocalSocket>
#include <QMainWindow>
#include <QTimer>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTreeWidget;

class MainWindow final : public QMainWindow {
public:
    MainWindow(QString endpoint, bool startCore);
    ~MainWindow() override;

private:
    void connectCore();
    void readMessages();
    void receive(const QJsonObject& message);
    void updateState(const QJsonObject& message);
    void requestFiles();
    void showFiles(const QJsonArray& entries);
    void updateDownloadButtons();
    void send(QJsonObject command);

    QString endpoint_;
    bool startCore_;
    bool attemptedStart_ = false;
    QLocalSocket socket_;
    QTimer reconnect_;
    QString selectedPeer_;
    QString listingToken_;
    QString sharedPath_;
    QString downloadsPath_;
    QJsonArray lastPeers_;
    QJsonArray downloads_;
    QLabel* connectionStatus_;
    QLabel* fileStatus_;
    QLineEdit* deviceId_;
    QLineEdit* name_;
    QPushButton* saveName_;
    QPushButton* sharedFolder_;
    QPushButton* downloadsFolder_;
    QPushButton* refreshFiles_;
    QTableWidget* peers_;
    QTreeWidget* files_;
    QTableWidget* transfers_;
};

#include "MainWindow.h"
#include "Local/Protocol.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTreeWidget>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

MainWindow::MainWindow(QString endpoint, bool startCore)
    : endpoint_(std::move(endpoint)), startCore_(startCore) {
    setWindowTitle("Relay — File sharing");
    resize(1000, 800);
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    setCentralWidget(central);
    auto* heading = new QLabel("Relay", central);
    auto font = heading->font();
    font.setPointSize(24);
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);
    layout->addWidget(new QLabel("The core keeps sharing and downloading when this window is closed.", central));

    auto* device = new QGroupBox("This device", central);
    auto* form = new QFormLayout(device);
    deviceId_ = new QLineEdit(device);
    deviceId_->setReadOnly(true);
    form->addRow("Device ID", deviceId_);
    name_ = new QLineEdit(device);
    name_->setPlaceholderText("Choose a display name");
    saveName_ = new QPushButton("Save name", device);
    auto* nameRow = new QHBoxLayout;
    nameRow->addWidget(name_);
    nameRow->addWidget(saveName_);
    form->addRow("Display name", nameRow);
    connect(saveName_, &QPushButton::clicked, this, [this] {
        send({{"command", "setName"}, {"name", name_->text()}});
    });
    sharedFolder_ = new QPushButton("Shared folder", device);
    downloadsFolder_ = new QPushButton("Downloads folder", device);
    form->addRow("Shared", sharedFolder_);
    form->addRow("Downloads", downloadsFolder_);
    const auto openFolder = [this](const QString& path) {
        if (!path.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            QMessageBox::warning(this, "Could not open folder", path);
    };
    connect(sharedFolder_, &QPushButton::clicked, this, [this, openFolder] { openFolder(sharedPath_); });
    connect(downloadsFolder_, &QPushButton::clicked, this, [this, openFolder] { openFolder(downloadsPath_); });
    layout->addWidget(device);

    connectionStatus_ = new QLabel("Connecting to the background core…", central);
    layout->addWidget(connectionStatus_);
    auto* splitter = new QSplitter(Qt::Horizontal, central);
    peers_ = new QTableWidget(0, 2, splitter);
    peers_->setHorizontalHeaderLabels({"Device ID", "Address"});
    peers_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    peers_->verticalHeader()->hide();
    peers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    peers_->setSelectionBehavior(QAbstractItemView::SelectRows);
    peers_->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* browser = new QWidget(splitter);
    auto* browserLayout = new QVBoxLayout(browser);
    auto* browserHeading = new QHBoxLayout;
    fileStatus_ = new QLabel("Select a connected peer to browse its shared files.", browser);
    fileStatus_->setWordWrap(true);
    refreshFiles_ = new QPushButton("Refresh files", browser);
    refreshFiles_->setEnabled(false);
    browserHeading->addWidget(fileStatus_, 1);
    browserHeading->addWidget(refreshFiles_);
    browserLayout->addLayout(browserHeading);
    files_ = new QTreeWidget(browser);
    files_->setHeaderLabels({"Name", "Size", "Download"});
    files_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    files_->setColumnWidth(1, 100);
    files_->setColumnWidth(2, 120);
    browserLayout->addWidget(files_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 3);
    connect(refreshFiles_, &QPushButton::clicked, this, [this] { requestFiles(); });
    connect(peers_, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto rows = peers_->selectionModel()->selectedRows();
        const auto peer = rows.isEmpty() ? QString() : peers_->item(rows.first().row(), 0)->text();
        if (peer == selectedPeer_) return;
        selectedPeer_ = peer;
        requestFiles();
    });

    layout->addWidget(new QLabel("Downloads (kept by the background core)", central));
    transfers_ = new QTableWidget(0, 3, central);
    transfers_->setHorizontalHeaderLabels({"File", "Status", "Details"});
    transfers_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    transfers_->verticalHeader()->hide();
    transfers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(transfers_, 1);
    saveName_->setEnabled(false);

    socket_.setReadBufferSize(relay::local::MaxFrameSize + 1);
    connect(&socket_, &QLocalSocket::connected, this, [this] {
        attemptedStart_ = true; // An explicit core stop must not trigger a new auto-start.
        connectionStatus_->setText("Connected to background core");
        send({{"command", "state"}});
    });
    connect(&socket_, &QLocalSocket::readyRead, this, [this] { readMessages(); });
    connect(&socket_, &QLocalSocket::disconnected, this, [this] {
        saveName_->setEnabled(false);
        peers_->clearContents();
        peers_->setRowCount(0);
        lastPeers_ = {};
        selectedPeer_.clear();
        requestFiles();
        transfers_->setRowCount(0);
        downloads_ = {};
        connectionStatus_->setText("Core disconnected. Reconnecting…");
    });
    connect(&socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        connectionStatus_->setText("Waiting for background core: " + socket_.errorString());
        if (!startCore_ || attemptedStart_ ||
            (socket_.error() != QLocalSocket::ServerNotFoundError &&
             socket_.error() != QLocalSocket::ConnectionRefusedError)) return;
        attemptedStart_ = true;
        QString executable = QCoreApplication::applicationDirPath() + "/RelayDaemon";
#ifdef Q_OS_WIN
        executable += ".exe";
#endif
        const auto data = relay::local::dataDirectory();
        if (!QFileInfo::exists(executable) || !QDir().mkpath(data)) {
            connectionStatus_->setText("Cannot start RelayDaemon. Build it beside RelayGui.");
            return;
        }
        QProcess process;
        process.setProgram(executable);
        process.setArguments({"--socket", endpoint_, "--data-dir", data});
        process.setWorkingDirectory(QDir::currentPath());
        process.setStandardOutputFile(data + "/daemon.log", QIODevice::Append);
        process.setStandardErrorFile(data + "/daemon.log", QIODevice::Append);
        if (!process.startDetached()) statusBar()->showMessage("Could not start RelayDaemon: " + process.errorString());
        else statusBar()->showMessage("Starting background core. Log: " + data + "/daemon.log");
    });
    connect(&reconnect_, &QTimer::timeout, this, [this] { connectCore(); });
    reconnect_.start(1000);
    connectCore();
}

MainWindow::~MainWindow() {
    // QLocalSocket emits disconnected during destruction. Detach callbacks while
    // all of the window's fields still exist, before member destruction begins.
    reconnect_.stop();
    socket_.disconnect(this);
    socket_.abort();
}

void MainWindow::connectCore() {
    if (socket_.state() == QLocalSocket::UnconnectedState) socket_.connectToServer(endpoint_);
}

void MainWindow::send(QJsonObject command) {
    if (!command.contains("token")) command["token"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!relay::local::send(&socket_, command)) statusBar()->showMessage("Background core is unavailable.");
}

void MainWindow::readMessages() {
    while (socket_.canReadLine()) {
        const auto line = socket_.readLine(relay::local::MaxFrameSize + 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        if (!line.endsWith('\n') || error.error != QJsonParseError::NoError || !document.isObject()) {
            statusBar()->showMessage("Invalid response from background core.");
            socket_.abort();
            return;
        }
        receive(document.object());
    }
    if (socket_.bytesAvailable() >= relay::local::MaxFrameSize) socket_.abort();
}

void MainWindow::receive(const QJsonObject& message) {
    const auto type = message["type"].toString();
    if (type == "state") updateState(message);
    else if (type == "listing" && message["token"].toString() == listingToken_ &&
             message["peer"].toString() == selectedPeer_) {
        listingToken_.clear();
        refreshFiles_->setEnabled(true);
        showFiles(message["entries"].toArray());
    } else if (type == "error") {
        if (message["token"].toString() == listingToken_) {
            listingToken_.clear();
            fileStatus_->setText(message["error"].toString());
            refreshFiles_->setEnabled(!selectedPeer_.isEmpty());
        }
        statusBar()->showMessage(message["error"].toString());
    } else if (type == "nameSaved") {
        name_->setModified(false);
        statusBar()->showMessage("Device name saved. Discovery will advertise it on its next broadcast.");
    }
}

void MainWindow::updateState(const QJsonObject& message) {
    saveName_->setEnabled(true);
    deviceId_->setText(message["deviceId"].toString());
    if (!name_->hasFocus() && !name_->isModified()) name_->setText(message["deviceName"].toString());
    sharedPath_ = message["sharedFolder"].toString();
    downloadsPath_ = message["downloadsFolder"].toString();
    sharedFolder_->setText(sharedPath_);
    downloadsFolder_->setText(downloadsPath_);
    const auto peers = message["peers"].toArray();
    connectionStatus_->setText(QString("Background core running • TCP %1 • Connected peers: %2")
                              .arg(message["port"].toInt()).arg(peers.size()));
    if (peers != lastPeers_) {
        QSignalBlocker blocker(peers_);
        lastPeers_ = peers;
        peers_->setRowCount(peers.size());
        bool found = false;
        int row = 0;
        for (const auto& value : peers) {
            const auto peer = value.toObject();
            const auto id = peer["id"].toString();
            peers_->setItem(row, 0, new QTableWidgetItem(id));
            peers_->setItem(row, 1, new QTableWidgetItem(peer["address"].toString()));
            if (id == selectedPeer_) { peers_->selectRow(row); found = true; }
            ++row;
        }
        if (!found) {
            peers_->clearSelection();
            selectedPeer_.clear();
            requestFiles();
        }
    }
    const auto downloads = message["downloads"].toArray();
    if (downloads != downloads_) {
        downloads_ = downloads;
        transfers_->setRowCount(downloads.size());
        int row = 0;
        for (const auto& value : downloads) {
            const auto job = value.toObject();
            auto* file = new QTableWidgetItem(job["path"].toString());
            file->setToolTip("Peer: " + job["peer"].toString());
            transfers_->setItem(row, 0, file);
            transfers_->setItem(row, 1, new QTableWidgetItem(job["status"].toString()));
            transfers_->setItem(row++, 2, new QTableWidgetItem(job["error"].toString()));
        }
        updateDownloadButtons();
    }
}

void MainWindow::requestFiles() {
    listingToken_.clear();
    files_->clear();
    refreshFiles_->setEnabled(false);
    if (selectedPeer_.isEmpty()) {
        fileStatus_->setText("Select a connected peer to browse its shared files.");
        return;
    }
    listingToken_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    fileStatus_->setText("Loading shared files…");
    send({{"command", "list"}, {"peer", selectedPeer_}, {"token", listingToken_}});
}

void MainWindow::showFiles(const QJsonArray& entries) {
    files_->clear();
    QMap<QString, QJsonObject> validated;
    // Validate the whole listing before creating any controls for untrusted paths.
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        const auto path = entry["path"].toString();
        const auto parts = path.split('/');
        const auto type = entry["type"].toString();
        if (path.isEmpty() || path.startsWith('/') || path.contains('\\') || path.contains(':') ||
            path.contains(QChar::Null) || parts.contains("..") || parts.contains(".") || parts.contains("") ||
            validated.contains(path) || (type != "file" && type != "directory" && type != "symlink" && type != "other") ||
            (type == "file" && entry["size"].toInteger(-1) < 0)) {
            fileStatus_->setText("Peer returned an invalid file tree.");
            return;
        }
        validated[path] = entry;
    }
    for (auto it = validated.cbegin(); it != validated.cend(); ++it) {
        auto parent = it.key().section('/', 0, -2);
        while (!parent.isEmpty()) {
            if (validated.contains(parent) && validated[parent]["type"] != "directory") {
                fileStatus_->setText("Peer returned an invalid file tree.");
                return;
            }
            parent = parent.section('/', 0, -2);
        }
    }
    QMap<QString, QTreeWidgetItem*> nodes;
    const auto ensureNode = [&](const QString& path) {
        auto* parent = files_->invisibleRootItem();
        QString prefix;
        for (const auto& part : path.split('/')) {
            if (!prefix.isEmpty()) prefix += '/';
            prefix += part;
            if (!nodes.contains(prefix)) {
                auto* item = new QTreeWidgetItem(parent, {part});
                item->setToolTip(0, prefix);
                nodes[prefix] = item;
            }
            parent = nodes[prefix];
        }
        return parent;
    };
    for (auto it = validated.cbegin(); it != validated.cend(); ++it) {
        auto* item = ensureNode(it.key());
        const auto type = it.value()["type"].toString();
        if (type == "file") {
            item->setText(1, QString::number(it.value()["size"].toInteger()) + " B");
            auto* download = new QPushButton("Download", files_);
            download->setProperty("path", it.key());
            const auto peer = selectedPeer_;
            const auto path = it.key();
            connect(download, &QPushButton::clicked, this, [this, download, peer, path] {
                download->setEnabled(false);
                send({{"command", "download"}, {"peer", peer}, {"path", path}});
                // The next service state supplies the authoritative transfer status.
                QTimer::singleShot(1500, this, [this] { updateDownloadButtons(); });
            });
            files_->setItemWidget(item, 2, download);
        } else if (type != "directory") item->setText(1, type);
    }
    files_->expandToDepth(0);
    fileStatus_->setText(entries.isEmpty() ? "This peer's shared folder is empty."
                                          : "Shared files — use the Downloads folder shortcut to find saved files.");
    updateDownloadButtons();
}

void MainWindow::updateDownloadButtons() {
    for (auto* button : files_->findChildren<QPushButton*>()) {
        const auto path = button->property("path").toString();
        bool active = false;
        bool completed = false;
        for (const auto& value : downloads_) {
            const auto job = value.toObject();
            if (job["path"] != path) continue;
            active |= job["status"] == "Downloading";
            completed |= job["status"] == "Completed" && job["peer"] == selectedPeer_;
        }
        button->setEnabled(!active && !completed && socket_.state() == QLocalSocket::ConnectedState);
        button->setText(active ? "Downloading…" : completed ? "Downloaded" : "Download");
    }
}

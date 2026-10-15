//
// Created by catcherpearce on 10/15/26.
//

#include "DiscoveryManager.h"
#include "../ConnectionManager.h"

#include <boost/asio.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <qstring.h>
#include <iostream>
#include <utility>

DiscoveryManager::DiscoveryManager(boost::asio::io_context& context,
                                   ConnectionManager& connectionManager,
                                   std::string deviceId,
                                   unsigned short tcpPort)
    : socket_(context), timer_(context), receiveBuffer_{},
      broadcastEndpoint_(boost::asio::ip::address_v4::broadcast(), 12346),
      deviceId_(std::move(deviceId)), tcpPort_(tcpPort),
      connectionManager_(connectionManager) {}

void DiscoveryManager::start() {
    if (!stopped_) return;
    socket_.open(boost::asio::ip::udp::v4());
    socket_.set_option(boost::asio::socket_base::broadcast(true));
    socket_.bind({boost::asio::ip::udp::v4(), 12346});
    stopped_ = false;
    receive();
    broadcast();
    scheduleBroadcast();
}

void DiscoveryManager::broadcast() {
    const auto message = QJsonDocument(QJsonObject{
    {"app", "relay"},
    {"version", 1},
    {"type", "discover"},
    {"deviceId", QString::fromStdString(deviceId_)},
    {"tcpPort", static_cast<int>(tcpPort_)}
    }).toJson(QJsonDocument::Compact);

    boost::system::error_code error;
    socket_.send_to(boost::asio::buffer(message.constData(), message.size()), broadcastEndpoint_, 0, error);
    if (error) std::cerr << "Discovery broadcast failed: " << error.message() << std::endl;
}

void DiscoveryManager::scheduleBroadcast() {
    timer_.expires_after(std::chrono::seconds(5));
    timer_.async_wait([this](boost::system::error_code error) {
        if (error || stopped_) return;

        broadcast();
        scheduleBroadcast();
    });
}

void DiscoveryManager::receive() {
    socket_.async_receive_from(
        boost::asio::buffer(receiveBuffer_),
        senderEndpoint_,
        [this](boost::system::error_code error, std::size_t bytesReceived) {
            if (stopped_) return;

            if (!error) {
                const QByteArray payload(receiveBuffer_.data(), static_cast<qsizetype>(bytesReceived));
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(payload, &parseError);

                if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                    const auto message = document.object();
                    const auto type = message.value("type").toString();
                    const auto peerId = message.value("deviceId");
                    const auto port = message.value("tcpPort").toInteger(-1);

                    if (message.value("app").toString() == "relay" &&
                        message.value("version").toInteger(-1) == 1 &&
                        (type == "discover") &&
                        peerId.isString() && !peerId.toString().isEmpty() &&
                        !peerId.toString().contains(QChar::Null) &&
                        peerId.toString().toStdString() != deviceId_ &&
                        port >= 1 && port <= 65535) {
                        connectionManager_.sendConnectionRequest(
                            senderEndpoint_.address().to_string(), static_cast<unsigned short>(port));
                    }
                }
            }

            if (error != boost::asio::error::operation_aborted) {
                receive();
            }
        });
}

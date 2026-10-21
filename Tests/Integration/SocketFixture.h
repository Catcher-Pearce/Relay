#pragma once

#include "Connection/ConnectionSession.h"
#include "TestFiles.h"
#include <boost/asio.hpp>
#include <QElapsedTimer>
#include <QtTest>

namespace test {
using tcp = boost::asio::ip::tcp;
using Type = ConnectionSession::MessageType;
using Message = ConnectionSession::Message;
struct SocketFixture {
    Files files;
    boost::asio::io_context context;
    tcp::socket remote{context};
    std::shared_ptr<ConnectionSession> session;
    SocketFixture() {
        tcp::acceptor listener(context, tcp::endpoint(tcp::v4(), 0));
        tcp::socket local(context);
        local.connect({boost::asio::ip::make_address("127.0.0.1"), listener.local_endpoint().port()});
        listener.accept(remote);
        remote.non_blocking(true);
        session = std::make_shared<ConnectionSession>(std::move(local),
            ConnectionSession::Direction::Outgoing, 1, nullptr, nullptr);
    }
    ~SocketFixture() { session->close(); context.stop(); }
    // Poll while reading so backpressure cannot deadlock a test. Every read is bounded.
    std::vector<std::uint8_t> readBytes(std::size_t count) {
        std::vector<std::uint8_t> data(count);
        std::size_t offset = 0;
        QElapsedTimer timeout; timeout.start();
        while (offset < count) {
            context.restart(); context.poll();
            boost::system::error_code error;
            offset += remote.read_some(boost::asio::buffer(data.data() + offset, count - offset), error);
            if (error && error != boost::asio::error::would_block && error != boost::asio::error::try_again)
                throw boost::system::system_error(error);
            if (timeout.elapsed() > 2000) throw std::runtime_error("Timed out waiting for frame");
            if (error) QTest::qSleep(1);
        }
        return data;
    }
    Message readFrame() {
        const auto header = readBytes(13);
        std::uint64_t id = 0;
        std::uint32_t count = 0;
        for (int i = 1; i < 9; ++i) id = (id << 8) | header[i];
        for (int i = 9; i < 13; ++i) count = (count << 8) | header[i];
        if (count > 1024 * 1024) throw std::runtime_error("Invalid frame length");
        return {static_cast<Type>(header[0]), id, readBytes(count)};
    }
    void poll() { context.restart(); context.poll(); }
};
}

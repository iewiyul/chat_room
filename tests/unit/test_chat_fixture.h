// ============================================================
// 文件: tests/unit/test_chat_fixture.h
// 作用: 1.6 测试共享 fixture（ChatRoom + Peer）
//
// 用法:
//   #include "test_chat_fixture.h"
//   ChatRoom room;
//   int a = room.addPeer();
//   room.sendFrom(a, {MSG_LOGIN, R"({"username":"alice"})"});
//   auto pkts = room.drain(a, 500);
//   auto ack = ChatRoom::findType(pkts, MSG_LOGIN_ACK);
//
// 构建/运行: 由顶层 CMakeLists.txt 管理
//   cmake -B build && cmake --build build && ctest --test-dir build
// ============================================================

#pragma once

#include "server/reactor.h"
#include "tcp/connection.h"
#include "tcp/user_manager.h"
#include "tcp/message_handler.h"
#include "tcp/connection_manager.h"
#include "common/utils.h"
#include "protocol/packet.h"
#include "protocol/message.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace chat::test {

using nlohmann::json;
using chat::server::Reactor;
using chat::server::Connection;
using chat::server::UserManager;
using chat::server::MessageHandler;
using chat::server::ConnectionManager;
using chat::protocol::Packet;
using chat::common::setNonBlocking;

struct Peer {
    Connection* conn;
    int         clientFd;
};

// 一个完整的"聊天服务端"实例：Reactor + UserManager + MessageHandler + 若干 Peer
// 析构时停 reactor，关闭所有 conn 和 clientFd。
struct ChatRoom {
    Reactor         reactor;
    std::thread     loopThread;
    UserManager     users;
    MessageHandler  handler;
    ConnectionManager connMgr;
    std::vector<Peer> peers;

    ChatRoom() : handler(users) {
        Reactor::TickCallback cb = [this]{ connMgr.tick(); };
        reactor.setTickCallback(cb);
        loopThread = std::thread([this]{ reactor.loop(); });
        // ⚠️ Reactor::loop() 把 running_ 写回 true；如果 stop() 在 loop()
        // 跑起来之前调用，会被覆盖、线程永不退出。先睡 10ms 让 loop() 启动。
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~ChatRoom() {
        reactor.stop();
        if (loopThread.joinable()) loopThread.join();
        for (auto& p : peers) {
            if (p.conn) {
                p.conn->close();
                delete p.conn;
            }
            if (p.clientFd >= 0) ::close(p.clientFd);
        }
    }

    // 新建一个"客户端"，返回 peer 索引
    int addPeer() {
        int fds[2];
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        setNonBlocking(fds[0]);
        setNonBlocking(fds[1]);

        Connection* c = new Connection(fds[0], reactor);
        c->setPacketCallback([this](Connection* conn, const Packet& p) {
            handler.onPacket(conn, p);
        });
        // close 流程：handler.onClose 负责广播"离线"+ 广播 USER_LIST + 从 users 移除；
        //                connMgr.remove 把它从心跳检测里摘掉
        c->setCloseCallback([this](Connection* conn) {
            handler.onClose(conn);
            connMgr.remove(conn);
        });
        c->start();
        connMgr.add(c);

        peers.push_back({c, fds[1]});
        return (int)peers.size() - 1;
    }

    Connection* conn(int i) { return peers[i].conn; }
    int         fd(int i)   { return peers[i].clientFd; }

    // 从 peer 的 clientFd 发出一个 packet
    void sendFrom(int i, const Packet& pkt) {
        auto bytes = chat::protocol::encode(pkt);
        ::send(peers[i].clientFd, bytes.data(), bytes.size(), 0);
    }

    // 在 waitMs 窗口内收集 peer 收到的所有 packet
    std::vector<Packet> drain(int i, int waitMs = 300) {
        std::vector<Packet> result;
        std::vector<uint8_t> buf;
        uint8_t tmp[4096];
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(waitMs);
        while (std::chrono::steady_clock::now() < deadline) {
            ssize_t n = ::recv(peers[i].clientFd, tmp, sizeof(tmp), MSG_DONTWAIT);
            if (n > 0) {
                buf.insert(buf.end(), tmp, tmp + n);
                while (buf.size() >= 5) {
                    auto r = chat::protocol::decode(buf.data(), buf.size());
                    if (!r.ok) break;
                    result.push_back(r.packet);
                    buf.erase(buf.begin(), buf.begin() + r.consumed);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        return result;
    }

    // 在包列表里找第一个指定类型的包；找不到返回 nullptr
    static const Packet* findType(const std::vector<Packet>& pkts, uint8_t type) {
        for (auto& p : pkts) {
            if (p.type == type) return &p;
        }
        return nullptr;
    }

    // 发送 LOGIN 并返回这次 drain 收集到的全部 packet
    // （ACK / SYSTEM / USER_LIST 都在里面，用 findType 找）
    std::vector<Packet> loginAs(int i, const std::string& username) {
        json j;
        j[chat::protocol::field::USERNAME] = username;
        sendFrom(i, {chat::protocol::MSG_LOGIN, j.dump()});
        return drain(i, 500);
    }
};

} // namespace chat::test
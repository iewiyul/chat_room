// ============================================================
// 文件: tests/unit/test_connection.cpp
// 作用: 1.5 Connection 类的测试
//
// 构建/运行: 由顶层 CMakeLists.txt 管理
//   cmake -B build && cmake --build build && ctest --test-dir build
// ============================================================

#include "tcp/connection.h"
#include "server/reactor.h"
#include "common/utils.h"
#include "protocol/packet.h"
#include "protocol/message.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace chat::server;
using namespace chat::common;
using namespace chat::protocol;

// ---------- 测试基础设施 ----------

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do {                                                   \
    if (!(cond)) {                                                         \
        printf("    ❌ %s:%d  CHECK(%s) 失败\n",                            \
               __FILE__, __LINE__, #cond);                                 \
        g_fail++;                                                          \
        return;                                                            \
    }                                                                      \
} while (0)

#define CHECK_EQ(a, b) do {                                                \
    auto _a = (a); auto _b = (b);                                          \
    if (!(_a == _b)) {                                                     \
        printf("    ❌ %s:%d  %s != %s\n",                                  \
               __FILE__, __LINE__, #a, #b);                                \
        g_fail++;                                                          \
        return;                                                            \
    }                                                                      \
} while (0)

#define RUN(fn) do {                                                       \
    int before = g_fail;                                                   \
    fn();                                                                  \
    if (g_fail == before) { printf("  ✅ %s\n", #fn); g_pass++; }           \
    else               { printf("  ❌ %s\n", #fn); }                       \
} while (0)

namespace {

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 1000) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// 用 socketpair 模拟一对连接：serverFd 交给 Connection 管，clientFd 留给测试用。
// 比 TCP loopback 简洁，没有端口冲突。
struct ConnFixture {
    Reactor     reactor;
    std::thread loopThread;
    Connection* conn = nullptr;
    int         clientFd = -1;
    bool        ownsClientFd = true;

    ConnFixture() {
        int fds[2];
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        int serverFd = fds[0];
        clientFd = fds[1];
        setNonBlocking(serverFd);
        setNonBlocking(clientFd);
        conn = new Connection(serverFd, reactor);
        conn->start();
        loopThread = std::thread([this]{ reactor.loop(); });
    }

    ~ConnFixture() {
        reactor.stop();
        if (loopThread.joinable()) loopThread.join();
        if (conn) {
            conn->close();      // removeFd + ::close(serverFd)
            delete conn;
        }
        if (ownsClientFd && clientFd >= 0) {
            ::close(clientFd);
        }
    }
};

} // namespace

// ---------- 1. 状态 / 用户名 / fd 访问器 ----------

void test_state_and_username() {
    Reactor reactor;
    int fds[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    setNonBlocking(fds[0]);
    Connection conn(fds[0], reactor);

    CHECK_EQ(conn.state(), Connection::CONNECTING);
    CHECK_EQ(conn.username(), std::string(""));
    CHECK_EQ(conn.fd(), fds[0]);

    conn.setUsername("alice");
    CHECK_EQ(conn.state(), Connection::AUTHENTICATED);
    CHECK_EQ(conn.username(), std::string("alice"));

    ::close(fds[1]);
    // conn 析构时会 close(fds[0]) + removeFd
}

// ---------- 2. lastActiveTime 在构造时初始化 ----------

void test_last_active_initialized() {
    Reactor reactor;
    int fds[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    setNonBlocking(fds[0]);
    Connection conn(fds[0], reactor);

    // 构造函数会调用 touch()，所以 lastActiveTime 应该 > 0
    CHECK(conn.lastActiveTime() > 0);

    ::close(fds[1]);
}

// ---------- 3. 收到一个完整包 ----------

void test_recv_complete_packet() {
    ConnFixture fx;
    std::atomic<int> count{0};
    Packet received{0, ""};

    fx.conn->setPacketCallback([&](Connection*, const Packet& p) {
        received = p;
        count++;
    });

    Packet pkt{MSG_CHAT, R"({"content":"hi"})"};
    auto bytes = encode(pkt);
    ssize_t sent = ::send(fx.clientFd, bytes.data(), bytes.size(), 0);
    CHECK_EQ(sent, (ssize_t)bytes.size());

    bool ok = waitFor([&]{ return count.load() == 1; }, 1000);
    CHECK(ok);
    CHECK_EQ(count.load(), 1);
    CHECK_EQ(received.type, MSG_CHAT);
    CHECK_EQ(received.body, std::string(R"({"content":"hi"})"));
}

// ---------- 4. 半包：字节逐个到达 ----------

void test_recv_half_packet() {
    ConnFixture fx;
    std::atomic<int> count{0};
    Packet received{0, ""};

    fx.conn->setPacketCallback([&](Connection*, const Packet& p) {
        received = p;
        count++;
    });

    Packet pkt{MSG_CHAT, R"({"content":"hello world"})"};
    auto bytes = encode(pkt);

    // 一字节一字节地发
    for (size_t i = 0; i < bytes.size(); i++) {
        ssize_t s = ::send(fx.clientFd, &bytes[i], 1, 0);
        CHECK_EQ(s, ssize_t(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    bool ok = waitFor([&]{ return count.load() == 1; }, 2000);
    CHECK(ok);
    CHECK_EQ(received.type, MSG_CHAT);
    CHECK_EQ(received.body, std::string(R"({"content":"hello world"})"));
}

// ---------- 5. 粘包：一次 send 塞进多个包 ----------

void test_recv_sticky_packets() {
    ConnFixture fx;
    std::atomic<int> count{0};
    Packet p1{0, ""}, p2{0, ""}, p3{0, ""};
    std::atomic<int> idx{0};

    fx.conn->setPacketCallback([&](Connection*, const Packet& p) {
        int i = idx.fetch_add(1);
        if      (i == 0) p1 = p;
        else if (i == 1) p2 = p;
        else if (i == 2) p3 = p;
        count++;
    });

    Packet a{MSG_LOGIN,      R"({"username":"alice"})"};
    Packet b{MSG_CHAT,       R"({"content":"hello"})"};
    Packet c{MSG_CHAT_BROAD, R"({"from":"bob","content":"hi"})"};

    auto ea = encode(a);
    auto eb = encode(b);
    auto ec = encode(c);

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), ea.begin(), ea.end());
    combined.insert(combined.end(), eb.begin(), eb.end());
    combined.insert(combined.end(), ec.begin(), ec.end());

    ssize_t sent = ::send(fx.clientFd, combined.data(), combined.size(), 0);
    CHECK_EQ(sent, (ssize_t)combined.size());

    bool ok = waitFor([&]{ return count.load() == 3; }, 2000);
    CHECK(ok);
    CHECK_EQ(count.load(), 3);
    CHECK_EQ(p1.type, MSG_LOGIN);
    CHECK_EQ(p1.body, std::string(R"({"username":"alice"})"));
    CHECK_EQ(p2.type, MSG_CHAT);
    CHECK_EQ(p2.body, std::string(R"({"content":"hello"})"));
    CHECK_EQ(p3.type, MSG_CHAT_BROAD);
    CHECK_EQ(p3.body, std::string(R"({"from":"bob","content":"hi"})"));
}

// ---------- 6. send 把数据发到对端 ----------

void test_send_echo() {
    ConnFixture fx;
    fx.conn->setPacketCallback(nullptr);

    Packet pkt{MSG_CHAT_BROAD, R"({"content":"server says hi"})"};
    fx.conn->send(pkt);

    // clientFd 是非阻塞的，用 MSG_DONTWAIT 轮询等 reactor 把数据写过来
    uint8_t buf[4096];
    ssize_t n = 0;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        n = ::recv(fx.clientFd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(n > 0);

    auto r = decode(buf, n);
    CHECK(r.ok);
    CHECK_EQ(r.packet.type, MSG_CHAT_BROAD);
    CHECK_EQ(r.packet.body, std::string(R"({"content":"server says hi"})"));
}

// ---------- 7. 对端关闭，状态变成 CLOSED ----------

void test_peer_close() {
    ConnFixture fx;
    fx.conn->setPacketCallback(nullptr);

    ::close(fx.clientFd);
    fx.ownsClientFd = false;   // 析构时不要再 close 一次

    bool ok = waitFor([&]{ return fx.conn->state() == Connection::CLOSED; }, 1000);
    CHECK(ok);
    CHECK_EQ(fx.conn->state(), Connection::CLOSED);
}

// ---------- 8. 多次 send 顺序保持 ----------

void test_multiple_sends_in_order() {
    ConnFixture fx;
    fx.conn->setPacketCallback(nullptr);

    const int N = 5;
    for (int i = 0; i < N; i++) {
        Packet pkt{MSG_CHAT, std::string("msg-") + std::to_string(i)};
        fx.conn->send(pkt);
    }

    // 非阻塞地循环读，每次把能解的包都解出来
    std::vector<Packet> received;
    std::vector<uint8_t> all;
    uint8_t buf[4096];

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline
           && received.size() < (size_t)N) {
        ssize_t n = ::recv(fx.clientFd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            all.insert(all.end(), buf, buf + n);
            while (all.size() >= 5) {  // 至少 4 字节头 + 1 字节 type
                auto r = decode(all.data(), all.size());
                if (!r.ok) break;
                received.push_back(r.packet);
                all.erase(all.begin(), all.begin() + r.consumed);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    CHECK_EQ(received.size(), (size_t)N);
    for (int i = 0; i < N; i++) {
        CHECK_EQ(received[i].type, MSG_CHAT);
        CHECK_EQ(received[i].body, std::string("msg-") + std::to_string(i));
    }
}

// ============================================================

int main() {
    printf("运行 Connection 单元测试...\n\n");

    RUN(test_state_and_username);
    RUN(test_last_active_initialized);
    RUN(test_recv_complete_packet);
    RUN(test_recv_half_packet);
    RUN(test_recv_sticky_packets);
    RUN(test_send_echo);
    RUN(test_peer_close);
    RUN(test_multiple_sends_in_order);

    printf("\n========== 结果 ==========\n");
    printf("通过: %d\n失败: %d\n", g_pass, g_fail);

    return g_fail == 0 ? 0 : 1;
}

// ============================================================
// 文件: tests/unit/test_reactor.cpp
// 作用: 1.4 Reactor 框架的测试
//
// 构建/运行: 由顶层 CMakeLists.txt 管理
//   cmake -B build && cmake --build build && ctest --test-dir build
// ============================================================

#include "server/reactor.h"
#include "common/utils.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace chat::server;
using namespace chat::common;

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

// 等待条件成立，最多 timeoutMs 毫秒
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

// RAII guard：保证 reactor 线程一定被 join
struct LoopGuard {
    Reactor*   r;
    std::thread* t;
    ~LoopGuard() {
        if (r) r->stop();
        if (t && t->joinable()) t->join();
    }
};

} // namespace

// ---------- 1. stop() 退出循环 ----------

void test_stop_exits_loop() {
    Reactor reactor;
    std::atomic<bool> exited{false};
    std::thread t([&]{ reactor.loop(); exited = true; });
    LoopGuard g{&reactor, &t};

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!exited);

    reactor.stop();
    bool ok = waitFor([&]{ return exited.load(); }, 2000);

    CHECK(ok);
    CHECK(exited);
}

// ---------- 2. EPOLLIN 触发回调 ----------

void test_read_event_fires() {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    setNonBlocking(fds[0]);

    Reactor reactor;
    std::atomic<int> callCount{0};
    std::string received;

    // 注意：pipe fd 上用 recv() 会 ENOTSOCK + busy loop，用 read()
    reactor.addFd(fds[0], EPOLLIN,
        [&]{
            char b;
            ::read(fds[0], &b, 1);
            received.push_back(b);
            callCount++;
        }, nullptr);

    std::thread t([&]{ reactor.loop(); });
    LoopGuard g{&reactor, &t};

    for (char c : std::string("abc")) {
        ::write(fds[1], &c, 1);
        bool ok = waitFor([&]{ return callCount.load() >= 1; }, 500);
        CHECK(ok);
        // 等待计数稳定（避免 busy loop 让下一个 iter 提前通过）
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ::close(fds[0]); ::close(fds[1]);

    CHECK_EQ(callCount.load(), 3);
    CHECK_EQ(received, std::string("abc"));
}

// ---------- 3. EPOLLOUT 触发回调 ----------

void test_write_event_fires() {
    int fds[2];
    ::pipe(fds);
    setNonBlocking(fds[1]);

    Reactor reactor;
    std::atomic<int> callCount{0};

    // EPOLLOUT 事件触发的是 onWrite 回调（不是 onRead）
    reactor.addFd(fds[1], EPOLLOUT, nullptr,
        [&]{
            callCount++;
            reactor.removeFd(fds[1]);
        });

    std::thread t([&]{ reactor.loop(); });
    LoopGuard g{&reactor, &t};

    bool ok = waitFor([&]{ return callCount.load() >= 1; }, 1000);

    ::close(fds[0]); ::close(fds[1]);

    CHECK(ok);
    CHECK(callCount >= 1);
}

// ---------- 4. 多个 fd 独立触发 ----------

void test_multiple_fds() {
    int f1[2], f2[2], f3[2];
    ::pipe(f1); ::pipe(f2); ::pipe(f3);
    setNonBlocking(f1[0]); setNonBlocking(f2[0]); setNonBlocking(f3[0]);

    Reactor reactor;
    std::atomic<int> hits{0}, hit1{0}, hit2{0}, hit3{0};

    reactor.addFd(f1[0], EPOLLIN,
        [&]{ char b; ::read(f1[0], &b, 1); hit1++; hits++; }, nullptr);
    reactor.addFd(f2[0], EPOLLIN,
        [&]{ char b; ::read(f2[0], &b, 1); hit2++; hits++; }, nullptr);
    reactor.addFd(f3[0], EPOLLIN,
        [&]{ char b; ::read(f3[0], &b, 1); hit3++; hits++; }, nullptr);

    std::thread t([&]{ reactor.loop(); });
    LoopGuard g{&reactor, &t};

    ::write(f1[1], "a", 1);
    ::write(f2[1], "b", 1);
    ::write(f3[1], "c", 1);

    bool ok = waitFor([&]{ return hits.load() >= 3; }, 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ::close(f1[0]); ::close(f1[1]);
    ::close(f2[0]); ::close(f2[1]);
    ::close(f3[0]); ::close(f3[1]);

    CHECK(ok);
    CHECK_EQ(hit1.load(), 1);
    CHECK_EQ(hit2.load(), 1);
    CHECK_EQ(hit3.load(), 1);
}

// ---------- 5. removeFd 之后不再触发 ----------

void test_remove_fd() {
    int fds[2];
    ::pipe(fds);
    setNonBlocking(fds[0]);

    Reactor reactor;
    std::atomic<int> callCount{0};

    reactor.addFd(fds[0], EPOLLIN,
        [&]{ char b; ::read(fds[0], &b, 1); callCount++; },
        nullptr);

    std::thread t([&]{ reactor.loop(); });
    LoopGuard g{&reactor, &t};

    ::write(fds[1], "x", 1);
    waitFor([&]{ return callCount.load() >= 1; }, 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int afterFirst = callCount.load();

    reactor.removeFd(fds[0]);

    ::write(fds[1], "y", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ::close(fds[0]); ::close(fds[1]);

    CHECK_EQ(afterFirst, 1);
    CHECK_EQ(callCount.load(), 1);  // removeFd 之后没新增
}

// ---------- 6. modifyFd 改变事件 ----------

void test_modify_fd() {
    int fds[2];
    ::pipe(fds);
    setNonBlocking(fds[0]);

    Reactor reactor;
    std::atomic<int> readHits{0}, writeHits{0};

    reactor.addFd(fds[0], EPOLLIN,
        [&]{ char b; ::read(fds[0], &b, 1); readHits++; },
        nullptr);

    std::thread t([&]{ reactor.loop(); });
    LoopGuard g{&reactor, &t};

    ::write(fds[1], "x", 1);
    waitFor([&]{ return readHits.load() >= 1; }, 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(readHits.load(), 1);

    reactor.modifyFd(fds[0], EPOLLOUT);

    ::write(fds[1], "y", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ::close(fds[0]); ::close(fds[1]);

    CHECK_EQ(readHits.load(), 1);
    CHECK_EQ(writeHits.load(), 0);  // 改 EPOLLOUT 后 read 不触发
}

// ---------- 7. EPOLLHUP（关闭对端） ----------

void test_peer_close() {
    int fds[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    setNonBlocking(fds[0]);

    Reactor reactor;
    std::atomic<int> callCount{0};

    reactor.addFd(fds[0], EPOLLIN,
        [&]{ char b; ::recv(fds[0], &b, 1, MSG_DONTWAIT); callCount++; },
        nullptr);

    std::thread t([&]{ reactor.loop(); });
    LoopGuard g{&reactor, &t};

    ::close(fds[1]);
    bool ok = waitFor([&]{ return callCount.load() >= 1; }, 1000);

    ::close(fds[0]);

    CHECK(ok);
}

// ============================================================
// 集成测试：真实 TCP echo
// ============================================================

namespace {

struct EchoServer {
    int listenFd;
    Reactor reactor;
    std::thread t;
    std::atomic<int> echoCount{0};

    EchoServer(int port) : listenFd(::socket(AF_INET, SOCK_STREAM, 0)) {
        int opt = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        ::bind(listenFd, (sockaddr*)&addr, sizeof(addr));
        ::listen(listenFd, 128);
        setNonBlocking(listenFd);

        reactor.addFd(listenFd, EPOLLIN,
            [&]{
                while (true) {
                    int cfd = ::accept(listenFd, nullptr, nullptr);
                    if (cfd < 0) break;
                    setNonBlocking(cfd);
                    reactor.addFd(cfd, EPOLLIN,
                        [this, cfd]{
                            char buf[4096];
                            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
                            if (n <= 0) {
                                ::close(cfd);
                                reactor.removeFd(cfd);
                                return;
                            }
                            ::send(cfd, buf, n, 0);
                            echoCount++;
                        }, nullptr);
                }
            }, nullptr);

        t = std::thread([this]{ reactor.loop(); });
    }

    ~EchoServer() {
        reactor.stop();
        if (t.joinable()) t.join();
        ::close(listenFd);
    }
};

// 连接到 echo server 并验证 echo
int connectTo(int port) {
    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(cfd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ::close(cfd);
        return -1;
    }
    return cfd;
}

} // namespace

void test_tcp_echo_single_client() {
    EchoServer server(19999);
    int cfd = connectTo(19999);
    CHECK(cfd >= 0);

    ::send(cfd, "hello world", 11, 0);
    char buf[64] = {};
    ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
    ::close(cfd);

    CHECK_EQ(n, ssize_t(11));
    CHECK_EQ(std::string(buf, n), std::string("hello world"));
}

void test_tcp_echo_multiple_messages() {
    EchoServer server(19998);
    int cfd = connectTo(19998);
    CHECK(cfd >= 0);

    for (int i = 0; i < 5; i++) {
        std::string msg = "msg-" + std::to_string(i);
        ::send(cfd, msg.data(), msg.size(), 0);
        char buf[64];
        ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
        CHECK_EQ(n, (ssize_t)msg.size());
        CHECK_EQ(std::string(buf, n), msg);
    }
    ::close(cfd);
}

void test_tcp_echo_multiple_clients() {
    EchoServer server(19997);

    int cfds[3];
    for (int i = 0; i < 3; i++) {
        cfds[i] = connectTo(19997);
        CHECK(cfds[i] >= 0);
    }

    const char* msgs[3] = {"client-A", "client-B", "client-C"};
    for (int i = 0; i < 3; i++) {
        ::send(cfds[i], msgs[i], strlen(msgs[i]), 0);
    }
    for (int i = 0; i < 3; i++) {
        char buf[64];
        ssize_t n = ::recv(cfds[i], buf, sizeof(buf), 0);
        CHECK_EQ(n, (ssize_t)strlen(msgs[i]));
        CHECK_EQ(std::string(buf, n), std::string(msgs[i]));
    }
    for (int i = 0; i < 3; i++) ::close(cfds[i]);
}

// ============================================================

int main() {
    printf("运行 Reactor 框架测试...\n\n");

    RUN(test_stop_exits_loop);
    RUN(test_read_event_fires);
    RUN(test_write_event_fires);
    RUN(test_multiple_fds);
    RUN(test_remove_fd);
    RUN(test_modify_fd);
    RUN(test_peer_close);
    RUN(test_tcp_echo_single_client);
    RUN(test_tcp_echo_multiple_messages);
    RUN(test_tcp_echo_multiple_clients);

    printf("\n========== 结果 ==========\n");
    printf("通过: %d\n失败: %d\n", g_pass, g_fail);

    return g_fail == 0 ? 0 : 1;
}
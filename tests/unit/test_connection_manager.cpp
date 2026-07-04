// ============================================================
// 文件: tests/unit/test_connection_manager.cpp
// 作用: 1.7 ConnectionManager + 心跳超时 + 离线广播测试
//
// 覆盖点：
//   - Reactor 的 tick 回调在 epoll_wait 超时（1s）时被触发
//   - 活跃连接（lastActive 是 now）不会被 tick 关闭
//   - 空闲连接（lastActive 远在过去）会被 tick 关闭
//   - 连接被关闭后，handler.onClose 会广播 MSG_SYSTEM("xxx离线") 给其它用户
//     并广播 MSG_USER_LIST（只剩其它用户）
//
// 构建/运行: 由顶层 CMakeLists.txt 管理
//   cmake -B build && cmake --build build && ctest --test-dir build
// ============================================================

#include "test_chat_fixture.h"
#include "tcp/connection_manager.h"
#include "protocol/packet.h"
#include "protocol/message.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace chat::test;
using chat::protocol::MSG_SYSTEM;
using chat::protocol::MSG_USER_LIST;
using chat::protocol::MSG_LOGIN_ACK;
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
    printf("  ▶ %s\n", #fn); fflush(stdout);                               \
    fn();                                                                  \
    if (g_fail == before) { printf("  ✅ %s\n", #fn); g_pass++; }           \
    else               { printf("  ❌ %s\n", #fn); }                       \
    fflush(stdout);                                                        \
} while (0)

// 等待 condition 满足，最多 waitMs 毫秒
static bool waitFor(int waitMs, std::function<bool()> cond) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(waitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// ---------- 1. Reactor tick 回调会在 1s 周期内触发多次 ----------

void test_tick_callback_fires() {
    Reactor reactor;
    std::atomic<int> ticks{0};
    Reactor::TickCallback cb = [&]{ ticks++; };
    reactor.setTickCallback(cb);
    std::thread t([&]{ reactor.loop(); });

    // 等 2.5s，至少能拿到 2 次 tick（每次 epoll_wait 1s 超时触发一次）
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    reactor.stop();
    if (t.joinable()) t.join();

    CHECK(ticks.load() >= 2);
}

// ---------- 2. 活跃连接不会被默认 30s timeout 关掉 ----------

void test_active_connection_not_closed() {
    ChatRoom room;
    int a = room.addPeer();

    // 刚连上的 conn，lastActive ≈ now，远没到 30s
    room.connMgr.tick();   // 默认 30s

    // 给 reactor 一点时间，但 conn 不应被关
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK_EQ(room.conn(a)->state(), Connection::CONNECTING);
}

// ---------- 3. 空闲连接（lastActive 在过去）会被 tick 关掉 ----------

void test_tick_closes_idle_connection() {
    ChatRoom room;
    int a = room.addPeer();

    // 等 1.1s 让 lastActive 落后至少 1 秒
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // 0s 超时：now - lastActive >= 1 > 0，关闭
    room.connMgr.tick(0);

    CHECK_EQ(room.conn(a)->state(), Connection::CLOSED);
}

// ---------- 4. 关闭已认证连接 → 其它用户收到"离线"系统消息 ----------

void test_close_broadcasts_offline() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();

    // alice / bob 登录
    auto pktsA = room.loginAs(a, "alice");
    auto pktsB = room.loginAs(b, "bob");

    // alice 这边：LOGIN_ACK(ok) + SYSTEM("alice上线") + USER_LIST([alice])
    auto ackA = ChatRoom::findType(pktsA, MSG_LOGIN_ACK);
    CHECK(ackA != nullptr);

    // 清掉登录阶段积攒的包
    room.drain(a, 200);
    room.drain(b, 200);

    // 模拟 alice 端掉线（关掉 clientFd，server 的 conn 收到 EPOLLHUP / read=0）
    ::close(room.fd(a));
    room.fd(a); // mark unused

    // bob 应该收到 MSG_SYSTEM + MSG_USER_LIST
    auto pkts = room.drain(b, 500);

    auto sys = ChatRoom::findType(pkts, MSG_SYSTEM);
    auto ulist = ChatRoom::findType(pkts, MSG_USER_LIST);

    CHECK(sys != nullptr);
    auto j = json::parse(sys->body);
    auto text = j[chat::protocol::field::CONTENT].get<std::string>();
    CHECK(text.find("alice") != std::string::npos);

    CHECK(ulist != nullptr);
    auto j2 = json::parse(ulist->body);
    auto names = j2[chat::protocol::field::USERS].get<std::vector<std::string>>();
    CHECK_EQ(names.size(), size_t(1));
    CHECK_EQ(names[0], std::string("bob"));
}

// ---------- 5. tick 同时关闭多个 idle 连接（验证 toClose 收集后批量关闭） ----------

void test_tick_closes_multiple_idle() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();
    int c = room.addPeer();

    // 等 1.1s 让所有 lastActive 都落后
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    room.connMgr.tick(0);

    // 三个全都被关
    CHECK_EQ(room.conn(a)->state(), Connection::CLOSED);
    CHECK_EQ(room.conn(b)->state(), Connection::CLOSED);
    CHECK_EQ(room.conn(c)->state(), Connection::CLOSED);
}

// ---------- 6. ConnectionManager add/remove（用行为验证） ----------

void test_connection_manager_add_remove() {
    ChatRoom room;
    int a = room.addPeer();

    // 在表里：等 1.1s 后 tick(0) 会关掉它
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    room.connMgr.tick(0);
    CHECK_EQ(room.conn(a)->state(), Connection::CLOSED);

    // remove 后：tick(0) 不会再操作它
    int b = room.addPeer();
    CHECK_EQ(room.conn(b)->state(), Connection::CONNECTING);
    room.connMgr.remove(room.conn(b));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    room.connMgr.tick(0);
    CHECK_EQ(room.conn(b)->state(), Connection::CONNECTING);
}

// ============================================================

int main() {
    printf("运行 ConnectionManager + 心跳/离线 单元测试...\n\n");

    RUN(test_tick_callback_fires);
    RUN(test_active_connection_not_closed);
    RUN(test_tick_closes_idle_connection);
    RUN(test_close_broadcasts_offline);
    RUN(test_tick_closes_multiple_idle);
    RUN(test_connection_manager_add_remove);

    printf("\n========== 结果 ==========\n");
    printf("通过: %d\n失败: %d\n", g_pass, g_fail);

    return g_fail == 0 ? 0 : 1;
}
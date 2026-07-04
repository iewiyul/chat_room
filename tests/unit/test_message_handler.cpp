// ============================================================
// 文件: tests/unit/test_message_handler.cpp
// 作用: 1.6 MessageHandler 单元测试
//
// 构建/运行: 由顶层 CMakeLists.txt 管理
//   cmake -B build && cmake --build build && ctest --test-dir build
// ============================================================

#include "test_chat_fixture.h"
#include "protocol/packet.h"
#include "protocol/message.h"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace chat::test;
using nlohmann::json;
using chat::protocol::Packet;
using chat::protocol::MessageType;
namespace field = chat::protocol::field;

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
bool stringContains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
} // namespace

// ---------- 1. LOGIN 成功 ----------

void test_login_success() {
    ChatRoom room;
    int a = room.addPeer();

    auto pkts = room.loginAs(a, "alice");
    const Packet* ack = ChatRoom::findType(pkts, MessageType::MSG_LOGIN_ACK);
    CHECK(ack != nullptr);

    json j = json::parse(ack->body);
    CHECK_EQ(j.value("ok", false), true);
    CHECK_EQ(room.conn(a)->state(), Connection::AUTHENTICATED);
    CHECK_EQ(room.conn(a)->username(), std::string("alice"));
    CHECK_EQ(room.users.size(), size_t(1));
}

// ---------- 2. 重名登录被拒 ----------

void test_login_duplicate_rejected() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();

    auto pkts1 = room.loginAs(a, "alice");
    const Packet* ack1 = ChatRoom::findType(pkts1, MessageType::MSG_LOGIN_ACK);
    CHECK(ack1 != nullptr);
    CHECK_EQ(json::parse(ack1->body).value("ok", false), true);

    auto pkts2 = room.loginAs(b, "alice");   // 同名
    const Packet* ack2 = ChatRoom::findType(pkts2, MessageType::MSG_LOGIN_ACK);
    CHECK(ack2 != nullptr);
    CHECK_EQ(json::parse(ack2->body).value("ok", true), false);

    CHECK_EQ(room.conn(b)->state(), Connection::CONNECTING);
    CHECK_EQ(room.users.size(), size_t(1));
    CHECK_EQ(room.users.get("alice"), room.conn(a));
}

// ---------- 3. LOGIN body 不是 JSON ----------

void test_login_invalid_json() {
    ChatRoom room;
    int a = room.addPeer();

    room.sendFrom(a, {MessageType::MSG_LOGIN, "not valid json {"});
    auto pkts = room.drain(a, 500);
    const Packet* ack = ChatRoom::findType(pkts, MessageType::MSG_LOGIN_ACK);
    CHECK(ack != nullptr);
    json j = json::parse(ack->body);
    CHECK_EQ(j.value("ok", true), false);
    CHECK_EQ(room.conn(a)->state(), Connection::CONNECTING);
}

// ---------- 4. LOGIN 用户名为空 ----------

void test_login_empty_username() {
    ChatRoom room;
    int a = room.addPeer();

    json j;
    j[field::USERNAME] = "";
    room.sendFrom(a, {MessageType::MSG_LOGIN, j.dump()});

    auto pkts = room.drain(a, 500);
    const Packet* ack = ChatRoom::findType(pkts, MessageType::MSG_LOGIN_ACK);
    CHECK(ack != nullptr);
    CHECK_EQ(json::parse(ack->body).value("ok", true), false);
}

// ---------- 5. 已登录后再 LOGIN 被拒 ----------

void test_login_already_authenticated_rejected() {
    ChatRoom room;
    int a = room.addPeer();

    auto pkts1 = room.loginAs(a, "alice");
    const Packet* ack1 = ChatRoom::findType(pkts1, MessageType::MSG_LOGIN_ACK);
    CHECK_EQ(json::parse(ack1->body).value("ok", false), true);

    auto pkts2 = room.loginAs(a, "bob");   // 换一个名字
    const Packet* ack2 = ChatRoom::findType(pkts2, MessageType::MSG_LOGIN_ACK);
    CHECK(ack2 != nullptr);
    CHECK_EQ(json::parse(ack2->body).value("ok", true), false);
    CHECK_EQ(room.conn(a)->username(), std::string("alice"));
}

// ---------- 6. LOGIN 后广播 SYSTEM + USER_LIST ----------

void test_login_broadcasts_system_and_userlist() {
    ChatRoom room;
    int a = room.addPeer();

    auto pkts = room.loginAs(a, "alice");

    const Packet* sys = ChatRoom::findType(pkts, MessageType::MSG_SYSTEM);
    CHECK(sys != nullptr);
    auto sysBody = json::parse(sys->body);
    CHECK(stringContains(sysBody[field::CONTENT].get<std::string>(), "alice"));

    const Packet* ulist = ChatRoom::findType(pkts, MessageType::MSG_USER_LIST);
    CHECK(ulist != nullptr);
    auto users = json::parse(ulist->body)[field::USERS].get<std::vector<std::string>>();
    CHECK_EQ(users.size(), size_t(1));
    CHECK_EQ(users[0], std::string("alice"));
}

// ---------- 7. CHAT 未登录被拒 ----------

void test_chat_requires_login() {
    ChatRoom room;
    int a = room.addPeer();

    json j;
    j[field::CONTENT] = "hi";
    room.sendFrom(a, {MessageType::MSG_CHAT, j.dump()});

    auto pkts = room.drain(a, 500);
    const Packet* ack = ChatRoom::findType(pkts, MessageType::MSG_LOGIN_ACK);
    CHECK(ack != nullptr);
    CHECK_EQ(json::parse(ack->body).value("ok", true), false);
}

// ---------- 8. CHAT 广播给其他人（不发回自己） ----------

void test_chat_broadcasts_to_others() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();

    auto pktsA1 = room.loginAs(a, "alice");   // 拿到 ACK + SYSTEM + USER_LIST
    auto pktsB1 = room.loginAs(b, "bob");     // 同上
    room.drain(a, 200);   // alice 还会再收到 bob 的 SYSTEM + USER_LIST

    json msg;
    msg[field::CONTENT] = "hello";
    room.sendFrom(a, {MessageType::MSG_CHAT, msg.dump()});

    auto pktsA = room.drain(a, 300);
    auto pktsB = room.drain(b, 300);

    // broadcast(pkt, conn) 排除发送者
    CHECK(ChatRoom::findType(pktsA, MessageType::MSG_CHAT_BROAD) == nullptr);
    const Packet* cb = ChatRoom::findType(pktsB, MessageType::MSG_CHAT_BROAD);
    CHECK(cb != nullptr);

    auto body = json::parse(cb->body);
    CHECK_EQ(body[field::FROM].get<std::string>(),   std::string("alice"));
    CHECK_EQ(body[field::CONTENT].get<std::string>(), std::string("hello"));
}

// ---------- 9. PRIVATE 只发给目标 ----------

void test_private_message_only_target() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();

    auto pktsA1 = room.loginAs(a, "alice");
    auto pktsB1 = room.loginAs(b, "bob");
    room.drain(a, 200);   // alice 收到 bob 的 SYSTEM + USER_LIST

    json msg;
    msg[field::TO]      = "bob";
    msg[field::CONTENT] = "secret";
    room.sendFrom(a, {MessageType::MSG_PRIVATE, msg.dump()});

    auto pktsA = room.drain(a, 300);
    auto pktsB = room.drain(b, 300);

    CHECK(ChatRoom::findType(pktsA, MessageType::MSG_PRIVATE) == nullptr);
    const Packet* priv = ChatRoom::findType(pktsB, MessageType::MSG_PRIVATE);
    CHECK(priv != nullptr);

    auto body = json::parse(priv->body);
    CHECK_EQ(body[field::FROM].get<std::string>(),   std::string("alice"));
    CHECK_EQ(body[field::TO].get<std::string>(),     std::string("bob"));
    CHECK_EQ(body[field::CONTENT].get<std::string>(), std::string("secret"));
}

// ---------- 10. PRIVATE 给不在线用户 → 错误 ack ----------

void test_private_to_offline_user() {
    ChatRoom room;
    int a = room.addPeer();
    auto pkts = room.loginAs(a, "alice");

    json msg;
    msg[field::TO]      = "nobody";
    msg[field::CONTENT] = "hi";
    room.sendFrom(a, {MessageType::MSG_PRIVATE, msg.dump()});

    auto resp = room.drain(a, 500);
    const Packet* err = ChatRoom::findType(resp, MessageType::MSG_LOGIN_ACK);
    CHECK(err != nullptr);
    CHECK_EQ(json::parse(err->body).value("ok", true), false);
}

// ---------- 11. 主动请求 USER_LIST ----------

void test_user_list_on_demand() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();

    auto pktsA = room.loginAs(a, "alice");
    auto pktsB = room.loginAs(b, "bob");
    room.drain(a, 200);   // alice 收到 bob 的更新

    room.sendFrom(a, {MessageType::MSG_USER_LIST, ""});
    auto resp = room.drain(a, 500);
    const Packet* ulist = ChatRoom::findType(resp, MessageType::MSG_USER_LIST);
    CHECK(ulist != nullptr);

    auto users = json::parse(ulist->body)[field::USERS].get<std::vector<std::string>>();
    CHECK_EQ(users.size(), size_t(2));
}

// ---------- 12. PING → PONG ⚠️ 这个会暴露 handlePing 的 bug ----------

void test_ping_returns_pong() {
    ChatRoom room;
    int a = room.addPeer();
    auto pkts = room.loginAs(a, "alice");

    room.sendFrom(a, {MessageType::MSG_PING, ""});
    auto resp = room.drain(a, 500);

    const Packet* pong = ChatRoom::findType(resp, MessageType::MSG_PONG);
    CHECK(pong != nullptr);  // ← 当前会失败：handler 回了 MSG_PING
}

// ---------- 13. LOGOUT 把用户从表中移除 ----------

void test_logout_removes_user() {
    ChatRoom room;
    int a = room.addPeer();
    auto pkts = room.loginAs(a, "alice");

    CHECK_EQ(room.users.size(), size_t(1));

    room.sendFrom(a, {MessageType::MSG_LOGOUT, ""});
    room.drain(a, 300);   // 收掉可能的广播

    CHECK_EQ(room.users.size(), size_t(0));
    CHECK(room.users.get("alice") == nullptr);
}

// ---------- 14. close 回调自动清理 ----------

void test_close_callback_removes_user() {
    ChatRoom room;
    int a = room.addPeer();
    auto pkts = room.loginAs(a, "alice");

    CHECK_EQ(room.users.size(), size_t(1));

    // 客户端断开 → reactor 检测到 EPOLLHUP → onClose → users.remove
    ::close(room.fd(a));

    bool ok = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline) {
        if (room.users.size() == 0) { ok = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(ok);
    CHECK_EQ(room.conn(a)->state(), Connection::CLOSED);
}

// ============================================================

int main() {
    printf("运行 MessageHandler 单元测试...\n\n");

    RUN(test_login_success);
    RUN(test_login_duplicate_rejected);
    RUN(test_login_invalid_json);
    RUN(test_login_empty_username);
    RUN(test_login_already_authenticated_rejected);
    RUN(test_login_broadcasts_system_and_userlist);
    RUN(test_chat_requires_login);
    RUN(test_chat_broadcasts_to_others);
    RUN(test_private_message_only_target);
    RUN(test_private_to_offline_user);
    RUN(test_user_list_on_demand);
    RUN(test_ping_returns_pong);
    RUN(test_logout_removes_user);
    RUN(test_close_callback_removes_user);

    printf("\n========== 结果 ==========\n");
    printf("通过: %d\n失败: %d\n", g_pass, g_fail);

    return g_fail == 0 ? 0 : 1;
}
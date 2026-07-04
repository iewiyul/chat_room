// ============================================================
// 文件: tests/unit/test_user_manager.cpp
// 作用: 1.6 UserManager 单元测试
//
// 构建/运行: 由顶层 CMakeLists.txt 管理
//   cmake -B build && cmake --build build && ctest --test-dir build
// ============================================================

#include "test_chat_fixture.h"
#include "protocol/packet.h"
#include "protocol/message.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace chat::test;
using chat::protocol::MSG_CHAT_BROAD;

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

// ---------- 1. add + get ----------

void test_add_and_get() {
    ChatRoom room;
    int a = room.addPeer();

    CHECK(room.users.add("alice", room.conn(a)));
    CHECK_EQ(room.users.get("alice"), room.conn(a));
    CHECK_EQ(room.users.size(), size_t(1));
}

// ---------- 2. 重名返回 false ----------

void test_duplicate_returns_false() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();

    CHECK(room.users.add("alice", room.conn(a)));
    CHECK(!room.users.add("alice", room.conn(b)));   // 重名
    CHECK(!room.users.add("alice", room.conn(a)));   // 自己重名也算
    CHECK_EQ(room.users.size(), size_t(1));
    CHECK_EQ(room.users.get("alice"), room.conn(a));  // 仍是第一个
}

// ---------- 3. 按用户名移除 ----------

void test_remove_by_name() {
    ChatRoom room;
    int a = room.addPeer();
    room.users.add("alice", room.conn(a));

    room.users.remove("alice");
    CHECK(room.users.get("alice") == nullptr);
    CHECK_EQ(room.users.size(), size_t(0));
}

// ---------- 4. 按 Connection* 移除 ----------

void test_remove_by_conn() {
    ChatRoom room;
    int a = room.addPeer();
    room.users.add("alice", room.conn(a));

    room.users.remove(room.conn(a));
    CHECK(room.users.get("alice") == nullptr);
    CHECK_EQ(room.users.size(), size_t(0));
}

// ---------- 5. 移除不存在的用户名/连接 ----------

void test_remove_nonexistent() {
    ChatRoom room;
    // 不应该崩
    room.users.remove("nobody");
    CHECK_EQ(room.users.size(), size_t(0));

    int a = room.addPeer();
    Connection* fake = nullptr;
    room.users.remove(fake);   // 传 nullptr
    CHECK_EQ(room.users.size(), size_t(0));
}

// ---------- 6. get 不存在的用户名 ----------

void test_get_nonexistent_returns_null() {
    ChatRoom room;
    CHECK(room.users.get("nobody") == nullptr);
}

// ---------- 7. size 累加 ----------

void test_size_after_operations() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();

    CHECK_EQ(room.users.size(), size_t(0));
    room.users.add("alice", room.conn(a));
    CHECK_EQ(room.users.size(), size_t(1));
    room.users.add("bob", room.conn(b));
    CHECK_EQ(room.users.size(), size_t(2));
    room.users.remove("alice");
    CHECK_EQ(room.users.size(), size_t(1));
    room.users.remove("bob");
    CHECK_EQ(room.users.size(), size_t(0));
}

// ---------- 8. usernames 列出所有人 ----------

void test_usernames_contains_all() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();
    room.users.add("alice", room.conn(a));
    room.users.add("bob",   room.conn(b));

    auto names = room.users.usernames();
    CHECK_EQ(names.size(), size_t(2));

    // unordered_map 顺序不定，检查包含关系
    bool hasAlice = std::find(names.begin(), names.end(), "alice") != names.end();
    bool hasBob   = std::find(names.begin(), names.end(), "bob")   != names.end();
    CHECK(hasAlice);
    CHECK(hasBob);
}

// ---------- 9. broadcast 给所有人 ----------

void test_broadcast_to_all() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();
    room.users.add("alice", room.conn(a));
    room.users.add("bob",   room.conn(b));

    Packet pkt{MSG_CHAT_BROAD, "hello-everyone"};
    room.users.broadcast(pkt);

    auto pktsA = room.drain(a, 200);
    auto pktsB = room.drain(b, 200);

    CHECK_EQ(pktsA.size(), size_t(1));
    CHECK_EQ(pktsB.size(), size_t(1));
    CHECK_EQ(pktsA[0].body, std::string("hello-everyone"));
    CHECK_EQ(pktsB[0].body, std::string("hello-everyone"));
}

// ---------- 10. broadcast 排除指定连接 ----------

void test_broadcast_excludes_sender() {
    ChatRoom room;
    int a = room.addPeer();
    int b = room.addPeer();
    room.users.add("alice", room.conn(a));
    room.users.add("bob",   room.conn(b));

    Packet pkt{MSG_CHAT_BROAD, "from-alice"};
    room.users.broadcast(pkt, room.conn(a));   // 排除 alice

    auto pktsA = room.drain(a, 200);
    auto pktsB = room.drain(b, 200);

    CHECK_EQ(pktsA.size(), size_t(0));        // alice 收不到
    CHECK_EQ(pktsB.size(), size_t(1));
    CHECK_EQ(pktsB[0].body, std::string("from-alice"));
}

// ============================================================

int main() {
    printf("运行 UserManager 单元测试...\n\n");

    RUN(test_add_and_get);
    RUN(test_duplicate_returns_false);
    RUN(test_remove_by_name);
    RUN(test_remove_by_conn);
    RUN(test_remove_nonexistent);
    RUN(test_get_nonexistent_returns_null);
    RUN(test_size_after_operations);
    RUN(test_usernames_contains_all);
    RUN(test_broadcast_to_all);
    RUN(test_broadcast_excludes_sender);

    printf("\n========== 结果 ==========\n");
    printf("通过: %d\n失败: %d\n", g_pass, g_fail);

    return g_fail == 0 ? 0 : 1;
}
// ============================================================
// 文件: tests/unit/test_packet.cpp
// 作用: 1.2 协议编解码的单元测试
//
// 构建/运行: 由顶层 CMakeLists.txt 管理
//   cmake -B build && cmake --build build && ctest --test-dir build
// ============================================================
// ⚠️ 重要：你的 packet.cpp 有未定义行为 bug，测试目前会全部失败
// 详见本文件末尾的「FAIL」注释
// ============================================================

#include "protocol/packet.h"
#include "protocol/message.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// ---------- 1. 基础 roundtrip ----------

void test_basic_roundtrip() {
    Packet pkt{MSG_CHAT, R"({"room":"lobby","content":"hello"})"};
    auto bytes = encode(pkt);

    CHECK_EQ(bytes.size(), HEADER_SIZE + TYPE_SIZE + pkt.body.size());

    auto r = decode(bytes.data(), bytes.size());
    CHECK(r.ok);
    CHECK_EQ(r.consumed, (int)bytes.size());
    CHECK_EQ(r.packet.type, MSG_CHAT);
    CHECK_EQ(r.packet.body, pkt.body);
}

// ---------- 2. 空 body（用 MSG_LOGOUT 占位） ----------

void test_empty_body() {
    Packet pkt{MSG_LOGOUT, ""};
    auto bytes = encode(pkt);

    CHECK_EQ(bytes.size(), HEADER_SIZE + TYPE_SIZE);  // 5 字节

    auto r = decode(bytes.data(), bytes.size());
    CHECK(r.ok);
    CHECK_EQ(r.packet.type, MSG_LOGOUT);
    CHECK(r.packet.body.empty());
}

// ---------- 3. 长 body（4KB） ----------

void test_long_body() {
    std::string longBody(4096, 'x');
    Packet pkt{MSG_CHAT, longBody};
    auto bytes = encode(pkt);

    auto r = decode(bytes.data(), bytes.size());
    CHECK(r.ok);
    CHECK_EQ(r.packet.body.size(), longBody.size());
    CHECK_EQ(r.packet.body, longBody);
}

// ---------- 4. 粘包 ----------

void test_sticky_packet() {
    Packet p1{MSG_LOGIN,  R"({"username":"alice"})"};
    Packet p2{MSG_CHAT,   R"({"content":"hi"})"};
    auto b1 = encode(p1);
    auto b2 = encode(p2);

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), b1.begin(), b1.end());
    combined.insert(combined.end(), b2.begin(), b2.end());

    auto r1 = decode(combined.data(), combined.size());
    CHECK(r1.ok);
    CHECK_EQ(r1.packet.type, MSG_LOGIN);
    CHECK_EQ(r1.packet.body, p1.body);

    auto r2 = decode(combined.data() + r1.consumed,
                     combined.size() - r1.consumed);
    CHECK(r2.ok);
    CHECK_EQ(r2.packet.type, MSG_CHAT);
    CHECK_EQ(r2.packet.body, p2.body);
}

// ---------- 5. 半包：长度字段都没收全 ----------

void test_partial_length() {
    uint8_t buf[3] = {0x00, 0x00, 0x00};
    auto r = decode(buf, 3);
    CHECK(!r.ok);
    CHECK_EQ(r.consumed, -1);
}

// ---------- 6. 半包：body 不全 ----------

void test_partial_body() {
    Packet pkt{MSG_CHAT, R"({"content":"hello world"})"};
    auto bytes = encode(pkt);

    size_t half = bytes.size() / 2;
    auto r = decode(bytes.data(), half);
    CHECK(!r.ok);
    CHECK_EQ(r.consumed, -1);
}

// ---------- 7. 一字节一字节到达 ----------

void test_byte_by_byte() {
    Packet pkt{MSG_CHAT, R"({"content":"hi"})"};
    auto bytes = encode(pkt);

    std::vector<uint8_t> buf;
    bool gotPacket = false;

    for (size_t i = 0; i < bytes.size(); i++) {
        buf.push_back(bytes[i]);
        auto r = decode(buf.data(), buf.size());
        if (r.ok) {
            CHECK_EQ(buf.size(), bytes.size());
            CHECK_EQ(r.packet.type, MSG_CHAT);
            CHECK_EQ(r.packet.body, pkt.body);
            gotPacket = true;
            break;
        } else {
            CHECK_EQ(r.consumed, -1);
        }
    }
    CHECK(gotPacket);
}

// ---------- 8. 粘包 + 半包混合 ----------

void test_sticky_and_partial() {
    Packet p1{MSG_CHAT,  R"({"content":"a"})"};
    Packet p2{MSG_LOGIN, R"({"username":"bob"})"};
    auto b1 = encode(p1);
    auto b2 = encode(p2);

    std::vector<uint8_t> buf;
    buf.insert(buf.end(), b1.begin(), b1.end());
    buf.insert(buf.end(), b2.begin(), b2.begin() + 5);

    auto r1 = decode(buf.data(), buf.size());
    CHECK(r1.ok);
    CHECK_EQ(r1.packet.type, MSG_CHAT);

    auto r2 = decode(buf.data() + r1.consumed,
                     buf.size() - r1.consumed);
    CHECK(!r2.ok);
    CHECK_EQ(r2.consumed, -1);

    buf.insert(buf.end(), b2.begin() + 5, b2.end());
    auto r3 = decode(buf.data() + r1.consumed,
                     buf.size() - r1.consumed);
    CHECK(r3.ok);
    CHECK_EQ(r3.packet.type, MSG_LOGIN);
    CHECK_EQ(r3.packet.body, p2.body);
}

// ---------- 9. 非法：长度超过上限 ----------

void test_oversized_length() {
    uint8_t buf[10];
    uint32_t bigLen = htonl(MAX_BODY_LENGTH + 1);  // 用你的常量名
    std::memcpy(buf, &bigLen, 4);
    buf[4] = MSG_CHAT;

    auto r = decode(buf, sizeof(buf));
    CHECK(!r.ok);
    CHECK_EQ(r.consumed, 0);
}

// ---------- 10. 非法：长度为 0 ----------

void test_zero_length() {
    uint8_t buf[5] = {0x00, 0x00, 0x00, 0x00, MSG_CHAT};
    auto r = decode(buf, 5);
    CHECK(!r.ok);
    CHECK_EQ(r.consumed, 0);
}

// ---------- 11. 所有 MessageType 都能 roundtrip（你目前定义的 8 个） ----------

void test_all_message_types() {
    const uint8_t types[] = {
        MSG_LOGIN, MSG_LOGIN_ACK, MSG_CHAT, MSG_CHAT_BROAD,
        MSG_USER_LIST, MSG_PRIVATE, MSG_PING, MSG_PONG,
        MSG_LOGOUT, MSG_SYSTEM
    };

    for (uint8_t t : types) {
        Packet pkt{t, R"({"k":"v"})"};
        auto bytes = encode(pkt);
        auto r = decode(bytes.data(), bytes.size());
        CHECK(r.ok);
        CHECK_EQ(r.packet.type, t);
    }
}

// ---------- 12. body 含特殊字符 ----------

void test_binary_body() {
    std::string body;
    body.push_back('\0');
    body.push_back('\xFF');
    body.push_back('\n');
    body.push_back('\r');
    body += "中文";

    Packet pkt{MSG_CHAT, body};
    auto bytes = encode(pkt);
    auto r = decode(bytes.data(), bytes.size());
    CHECK(r.ok);
    CHECK_EQ(r.packet.body, body);
}

// ---------- 13. 字节序是大端 ----------

void test_byte_order_is_big_endian() {
    Packet pkt{MSG_LOGIN, ""};
    auto bytes = encode(pkt);

    CHECK_EQ(bytes[0], 0x00);
    CHECK_EQ(bytes[1], 0x00);
    CHECK_EQ(bytes[2], 0x00);
    CHECK_EQ(bytes[3], 0x01);
    CHECK_EQ(bytes[4], MSG_LOGIN);
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("运行协议编解码测试...\n\n");

    RUN(test_basic_roundtrip);
    RUN(test_empty_body);
    RUN(test_long_body);
    RUN(test_sticky_packet);
    RUN(test_partial_length);
    RUN(test_partial_body);
    RUN(test_byte_by_byte);
    RUN(test_sticky_and_partial);
    RUN(test_oversized_length);
    RUN(test_zero_length);
    RUN(test_all_message_types);
    RUN(test_binary_body);
    RUN(test_byte_order_is_big_endian);

    printf("\n========== 结果 ==========\n");
    printf("通过: %d\n失败: %d\n", g_pass, g_fail);

    if (g_fail > 0) {
        printf("\n");
        printf("💡 提示：你的 packet.cpp 第 40-42 行有 bug：\n");
        printf("   uint32_t netLen, bodyLen;\n");
        printf("   memcpy(&bodyLen, data, HEADER_SIZE);  // 写入 bodyLen\n");
        printf("   bodyLen = ntohl(netLen);              // netLen 未初始化！\n");
        printf("\n");
        printf("   修复：先 memcpy 到 netLen，再 ntohl 转换\n");
        return 1;
    }

    printf("\n🎉 全部通过！1.2 完成。\n");
    return 0;
}
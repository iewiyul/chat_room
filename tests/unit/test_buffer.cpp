// ============================================================
// 文件: tests/unit/test_buffer.cpp
// 作用: 1.3 Buffer 类的单元测试
//
// 构建/运行: 由顶层 CMakeLists.txt 管理
//   cmake -B build && cmake --build build && ctest --test-dir build
// ============================================================

#include "common/buffer.h"
#include "protocol/packet.h"
#include "protocol/message.h"

#include <cstdio>
#include <cstring>
#include <string>

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

// ---------- 1. 初始状态 ----------

void test_initial_state() {
    Buffer buf;
    CHECK_EQ(buf.readableBytes(), size_t(0));
    CHECK(buf.writableBytes() >= Buffer::INITIAL_SIZE);
}

// ---------- 2. 基础 append + peek ----------

void test_basic_append() {
    Buffer buf;
    const char* data = "hello";
    buf.append(reinterpret_cast<const uint8_t*>(data), 5);

    CHECK_EQ(buf.readableBytes(), size_t(5));
    CHECK_EQ(std::memcmp(buf.peek(), "hello", 5), 0);
}

// ---------- 3. consume 后 peek 指向剩余 ----------

void test_consume() {
    Buffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("abcdef"), 6);

    CHECK_EQ(buf.readableBytes(), size_t(6));
    CHECK_EQ(std::memcmp(buf.peek(), "abcdef", 6), 0);

    buf.consume(3);
    CHECK_EQ(buf.readableBytes(), size_t(3));
    CHECK_EQ(std::memcmp(buf.peek(), "def", 3), 0);
}

// ---------- 4. 多次 append 累积 ----------

void test_multiple_appends() {
    Buffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("hel"), 3);
    buf.append(reinterpret_cast<const uint8_t*>("lo wo"), 5);
    buf.append(reinterpret_cast<const uint8_t*>("rld"), 3);

    CHECK_EQ(buf.readableBytes(), size_t(11));
    CHECK_EQ(std::memcmp(buf.peek(), "hello world", 11), 0);
}

// ---------- 5. consume 比 readable 大（防御性） ----------

void test_consume_overflow() {
    Buffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("abc"), 3);

    buf.consume(100);  // 超过 readableBytes
    CHECK_EQ(buf.readableBytes(), size_t(0));
}

// ---------- 6. writeBegin + hasWritten 模式 ----------

void test_write_begin_has_written() {
    Buffer buf;
    CHECK(buf.writableBytes() >= Buffer::INITIAL_SIZE);

    // 像 recv() 一样直接写入
    uint8_t* dst = buf.writeBegin();
    std::memcpy(dst, "xyz", 3);
    buf.hasWritten(3);

    CHECK_EQ(buf.readableBytes(), size_t(3));
    CHECK_EQ(std::memcmp(buf.peek(), "xyz", 3), 0);
}

// ---------- 7. 触发 makeSpace 的压缩路径 ----------

void test_compact() {
    Buffer buf;

    // 写入 800 字节
    std::string big(800, 'x');
    buf.append(reinterpret_cast<const uint8_t*>(big.data()), big.size());
    CHECK_EQ(buf.readableBytes(), size_t(800));

    // 消耗 700 字节，readPos_ = 700, writePos_ = 800, free = 224
    buf.consume(700);
    CHECK_EQ(buf.readableBytes(), size_t(100));

    // 再追加 200 字节：free=224 够装，理论上不需要压缩
    // 改成追加 250 字节触发压缩判断
    std::string extra(250, 'y');
    buf.append(reinterpret_cast<const uint8_t*>(extra.data()), extra.size());

    CHECK_EQ(buf.readableBytes(), size_t(350));  // 100 + 250

    // 验证数据完整性：前 100 是 x，后 250 是 y
    for (size_t i = 0; i < 100; i++) {
        CHECK_EQ(buf.peek()[i], (uint8_t)'x');
    }
    for (size_t i = 0; i < 250; i++) {
        CHECK_EQ(buf.peek()[100 + i], (uint8_t)'y');
    }
}

// ---------- 8. 触发扩容（压缩也不够） ----------

void test_expand() {
    Buffer buf;  // 初始 1024

    // 消耗一些，留出 readPos_
    std::string pre(100, 'a');
    buf.append(reinterpret_cast<const uint8_t*>(pre.data()), pre.size());
    buf.consume(100);  // readPos_ = 100

    // 写入 2048 字节，远超初始容量，需要扩容
    std::string huge(2048, 'z');
    buf.append(reinterpret_cast<const uint8_t*>(huge.data()), huge.size());

    CHECK_EQ(buf.readableBytes(), size_t(2048));
    for (size_t i = 0; i < 2048; i++) {
        CHECK_EQ(buf.peek()[i], (uint8_t)'z');
    }
}

// ---------- 9. clear ----------

void test_clear() {
    Buffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("some data"), 9);
    CHECK_EQ(buf.readableBytes(), size_t(9));

    buf.clear();
    CHECK_EQ(buf.readableBytes(), size_t(0));
}

// ---------- 10. string 类型的 append ----------

void test_append_string() {
    Buffer buf;
    std::string s = "hello world";
    buf.append(s);

    CHECK_EQ(buf.readableBytes(), size_t(11));
    CHECK_EQ(std::memcmp(buf.peek(), "hello world", 11), 0);
}

// ---------- 11. retrieveAllAsString ----------

void test_retrieve_all_as_string() {
    Buffer buf;
    buf.append(std::string("hello world"));

    std::string s = buf.retrieveAllAsString();
    CHECK_EQ(s, std::string("hello world"));
    CHECK_EQ(buf.readableBytes(), size_t(0));
}

// ---------- 12. 反复写入清空（压力测试） ----------

void test_repeated_cycles() {
    Buffer buf;

    for (int cycle = 0; cycle < 100; cycle++) {
        std::string data = "round " + std::to_string(cycle);
        buf.append(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK_EQ(buf.readableBytes(), data.size());
        CHECK_EQ(std::memcmp(buf.peek(), data.data(), data.size()), 0);
        buf.consume(data.size());
        CHECK_EQ(buf.readableBytes(), size_t(0));
    }
}

// ---------- 13. 和 decode 集成：模拟 recv 累积 ----------

void test_integration_with_decode() {
    Buffer buf;

    // 编码一个完整包
    Packet pkt{MSG_CHAT, R"({"content":"hi"})"};
    auto encoded = encode(pkt);

    // 模拟 recv 一字节一字节到达
    bool gotPacket = false;
    for (size_t i = 0; i < encoded.size(); i++) {
        buf.append(&encoded[i], 1);

        if (buf.readableBytes() >= HEADER_SIZE) {
            auto r = decode(buf.peek(), buf.readableBytes());
            if (r.ok) {
                CHECK_EQ(r.packet.body, pkt.body);
                buf.consume(r.consumed);
                CHECK_EQ(buf.readableBytes(), size_t(0));
                gotPacket = true;
                break;
            }
        }
    }
    CHECK(gotPacket);
}

// ---------- 14. 和 decode 集成：粘包场景 ----------

void test_integration_sticky_packets() {
    Buffer buf;

    Packet p1{MSG_LOGIN, R"({"username":"alice"})"};
    Packet p2{MSG_CHAT,  R"({"content":"hi"})"};
    auto e1 = encode(p1);
    auto e2 = encode(p2);

    // 一次性把两个包都塞进 buffer
    buf.append(e1.data(), e1.size());
    buf.append(e2.data(), e2.size());

    // 解第一个
    auto r1 = decode(buf.peek(), buf.readableBytes());
    CHECK(r1.ok);
    CHECK_EQ(r1.packet.body, p1.body);
    buf.consume(r1.consumed);

    // 解第二个（用剩余数据）
    auto r2 = decode(buf.peek(), buf.readableBytes());
    CHECK(r2.ok);
    CHECK_EQ(r2.packet.body, p2.body);
    buf.consume(r2.consumed);

    CHECK_EQ(buf.readableBytes(), size_t(0));
}

// ============================================================

int main() {
    printf("运行 Buffer 单元测试...\n\n");

    RUN(test_initial_state);
    RUN(test_basic_append);
    RUN(test_consume);
    RUN(test_multiple_appends);
    RUN(test_consume_overflow);
    RUN(test_write_begin_has_written);
    RUN(test_compact);
    RUN(test_expand);
    RUN(test_clear);
    RUN(test_append_string);
    RUN(test_retrieve_all_as_string);
    RUN(test_repeated_cycles);
    RUN(test_integration_with_decode);
    RUN(test_integration_sticky_packets);

    printf("\n========== 结果 ==========\n");
    printf("通过: %d\n失败: %d\n", g_pass, g_fail);

    if (g_fail > 0) {
        printf("\n💡 提示：你的 buffer.h 第 39 行 retrieveAllAsString 有 bug\n");
        printf("   当前代码:\n");
        printf("     std::string s(reinterpret_cast<const char*>(peek())+readableBytes());\n");
        printf("   期望:\n");
        printf("     std::string s(reinterpret_cast<const char*>(peek()), readableBytes());\n");
    }

    return g_fail == 0 ? 0 : 1;
}
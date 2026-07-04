#include "../../src/server/ws_frame.h"
#include <cstdio>
#include <string>
#include <vector>
#include <cassert>

using namespace chat::server::ws;

static int passed = 0, failed = 0;
#define CHECK(cond) do { \
    if (cond) { ++passed; } \
    else { ++failed; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } \
} while(0)

// 模拟客户端:把 srv 编好的帧加 mask (mkey 必须在 length 字段之后)
static std::vector<uint8_t> asClientMasked(const std::vector<uint8_t>& srv){
    std::vector<uint8_t> out;
    out.push_back(srv[0]);
    uint8_t b1 = srv[1] | 0x80;
    out.push_back(b1);

    size_t len_pos = 2;
    uint64_t plen = srv[1] & 0x7F;
    if (plen == 126){ out.push_back(srv[2]); out.push_back(srv[3]); len_pos = 4; }
    else if (plen == 127){ for (int i=0;i<8;++i) out.push_back(srv[2+i]); len_pos = 10; }

    uint8_t mk[4] = {0xAA,0xBB,0xCC,0xDD};
    for (int i=0;i<4;++i) out.push_back(mk[i]);
    for (size_t i=len_pos; i<srv.size(); ++i) out.push_back(srv[i] ^ mk[(i-len_pos)%4]);
    return out;
}

int main(){
    // ===== tryDecode =====
    {
        uint8_t raw[] = {0x81, 0x82, 0x01,0x02,0x03,0x04, 0x49,0x6b};
        auto r = tryDecode(raw, sizeof(raw));
        CHECK(r.consumed == 8);
        CHECK(r.frame.fin);
        CHECK(r.frame.opcode == TEXT);
        CHECK(r.frame.payload.size() == 2);
        CHECK(r.frame.payload[0] == 'H');
        CHECK(r.frame.payload[1] == 'i');
    }

    // 数据不足首部
    {
        uint8_t raw[] = {0x81};
        CHECK(tryDecode(raw, 1).consumed == -1);
    }

    // 中等长度(126 + 2字节扩展)
    {
        uint8_t raw[2+2+4+11] = {0x81, 0xFE, 0x00, 0x0B,
                                 0xAA,0xBB,0xCC,0xDD};
        uint8_t mk[4] = {0xAA,0xBB,0xCC,0xDD};
        const char* msg = "Hello World";
        for (int i=0; i<11; ++i) raw[8+i] = uint8_t(msg[i]) ^ mk[i%4];
        auto r = tryDecode(raw, sizeof(raw));
        CHECK(r.consumed == (int)sizeof(raw));
        CHECK(r.frame.payload.size() == 11);
        std::string s((char*)r.frame.payload.data(), r.frame.payload.size());
        CHECK(s == "Hello World");
    }

    // 客户端没 mask → 协议错
    {
        uint8_t raw[] = {0x81, 0x02, 'H', 'i'};
        CHECK(tryDecode(raw, sizeof(raw)).consumed == 0);
    }

    // PING 帧
    {
        uint8_t raw[] = {0x89, 0x84, 0x01,0x02,0x03,0x04,
                         'p'^0x01, 'i'^0x02, 'n'^0x03, 'g'^0x04};
        auto r = tryDecode(raw, sizeof(raw));
        CHECK(r.consumed == 10);
        CHECK(r.frame.opcode == PING);
        std::string s((char*)r.frame.payload.data(), r.frame.payload.size());
        CHECK(s == "ping");
    }

    // ===== encodeFrame =====
    {
        auto v = encodeText("Hi");
        CHECK(v.size() == 4);
        CHECK(v[0] == 0x81);
        CHECK(v[1] == 0x02);
        CHECK(v[2] == 'H');
        CHECK(v[3] == 'i');
    }

    {
        auto v = encodeText("");
        CHECK(v.size() == 2);
        CHECK(v[0] == 0x81);
        CHECK(v[1] == 0x00);
    }

    {
        std::string s(200, 'x');
        auto v = encodeText(s);
        CHECK(v.size() == 2 + 2 + 200);
        CHECK(v[0] == 0x81);
        CHECK(v[1] == 126);
        CHECK(v[2] == 0);
        CHECK(v[3] == 200);
    }

    {
        auto v = encodeClose();
        CHECK(v.size() == 2);
        CHECK(v[0] == 0x88);
        CHECK(v[1] == 0x00);
    }

    {
        auto v = encodeClose(1000);
        CHECK(v[0] == 0x88);
        CHECK(v[1] == 0x02);
        CHECK(v[2] == 0x03);
        CHECK(v[3] == 0xE8);
    }

    {
        std::string s(70000, 'a');
        auto v = encodeText(s);
        CHECK(v.size() == 2 + 8 + 70000);
        CHECK(v[1] == 127);
        CHECK(v[2] == 0); CHECK(v[3] == 0); CHECK(v[4] == 0); CHECK(v[5] == 0);
        CHECK(v[6] == 0); CHECK(v[7] == 1); CHECK(v[8] == 0x11); CHECK(v[9] == 0x70);
    }

    // ===== round-trip =====
    {
        auto roundtrip = [&](const std::string& s){
            auto enc = encodeText(s);
            auto wire = asClientMasked(enc);
            auto r = tryDecode(wire.data(), wire.size());
            CHECK(r.consumed == (int)wire.size());
            CHECK(r.frame.fin);
            CHECK(r.frame.opcode == TEXT);
            std::string got((char*)r.frame.payload.data(), r.frame.payload.size());
            CHECK(got == s);
        };
        roundtrip("Hi");
        roundtrip("");
        roundtrip(std::string(500, 'x'));
        roundtrip(std::string(70000, 'y'));
    }

    printf("\nws_frame: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
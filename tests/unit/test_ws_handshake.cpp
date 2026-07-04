#include "../../src/server/ws_handshake.h"
#include <cstdio>
#include <string>

using namespace chat::server::ws;

static int passed = 0, failed = 0;
#define CHECK(cond) do { \
    if (cond) { ++passed; } \
    else { ++failed; fprintf(stderr, "FAIL line %d\n", __LINE__); } \
} while(0)

int main(){
    // RFC 6455 §1.3 黄金测试向量
    {
        std::string got = computeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
        std::string expected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
        CHECK(got == expected);
    }

    // 长度永远是 28
    {
        auto got = computeAcceptKey("xqBt3ImNzJbYqRINxEFlkg==");
        CHECK(got.size() == 28);
    }

    // 不同 key → 不同 accept
    {
        auto a = computeAcceptKey("AAAAAAAAAAAAA==");
        auto b = computeAcceptKey("BBBBBBBBBBBBB==");
        CHECK(a != b);
    }

    // 确定性:同样输入永远同样输出
    {
        auto k = computeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
        for (int i=0; i<3; ++i){
            CHECK(computeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == k);
        }
    }

    printf("\nws_handshake: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
#include "../../src/server/ws_connection.h"
#include "../../src/server/ws_frame.h"
#include "../../src/server/reactor.h"

#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cassert>

using namespace chat::server;
using chat::common::Buffer;

static int passed = 0, failed = 0;
#define CHECK(cond) do { \
    if (cond) { ++passed; } \
    else { ++failed; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } \
} while(0)

static void sendAll(int fd, const std::vector<uint8_t>& data){
    size_t off = 0;
    while (off < data.size()){
        ssize_t n = ::send(fd, data.data()+off, data.size()-off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += n;
    }
}

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

static std::vector<uint8_t> recvBytes(int fd, int timeoutMs, size_t expected){
    std::vector<uint8_t> out;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char tmp[4096];
    while (out.size() < expected && std::chrono::steady_clock::now() < deadline){
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n > 0){ out.insert(out.end(), tmp, tmp+n); continue; }
        if (n == 0) break;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        usleep(2000);
    }
    return out;
}

int main(){
    // T1: 客户端 masked TEXT → server 触发 onText
    {
        int sv[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        Reactor r;
        std::string got;
        std::atomic<bool> fired{false};
        WsConnection conn(sv[1], r, "alice");
        conn.setTextHandler([&](WsConnection*, const std::string& s){
            got = s; fired = true;
        });
        conn.start();
        std::thread th([&]{ r.loop(); });
        usleep(30000);

        sendAll(sv[0], asClientMasked(ws::encodeText("hello")));
        for (int i=0; i<200 && !fired; ++i) usleep(5000);

        r.stop(); th.join(); close(sv[0]);
        CHECK(fired.load());
        CHECK(got == "hello");
    }

    // T2: server sendText → 客户端收到 unmasked 帧
    {
        int sv[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        Reactor r;
        WsConnection conn(sv[1], r, "alice");
        conn.start();
        std::thread th([&]{ r.loop(); });
        usleep(30000);

        conn.sendText("world");
        auto bytes = recvBytes(sv[0], 500, 7);

        r.stop(); th.join(); close(sv[0]);
        CHECK(bytes.size() == 7);
        if (bytes.size() == 7){
            CHECK(bytes[0] == 0x81);
            CHECK(bytes[1] == 0x05);
            CHECK(std::string((char*)bytes.data()+2, 5) == "world");
        }
    }

    // T3: PING → 自动回 PONG
    {
        int sv[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        Reactor r;
        WsConnection conn(sv[1], r, "alice");
        conn.start();
        std::thread th([&]{ r.loop(); });
        usleep(30000);

        sendAll(sv[0], asClientMasked(ws::encodePing("test-ping")));
        auto bytes = recvBytes(sv[0], 500, 11);

        r.stop(); th.join(); close(sv[0]);
        CHECK(bytes.size() == 11);
        if (bytes.size() == 11){
            CHECK(bytes[0] == 0x8A);
            CHECK(bytes[1] == 0x09);
            CHECK(std::string((char*)bytes.data()+2, 9) == "test-ping");
        }
    }

    // T4: 客户端发 CLOSE → server 回 ack + 触发 onClose
    {
        int sv[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        Reactor r;
        std::atomic<bool> closed{false};
        WsConnection conn(sv[1], r, "alice");
        conn.setCloseHandler([&](WsConnection*){ closed = true; });
        conn.start();
        std::thread th([&]{ r.loop(); });
        usleep(30000);

        sendAll(sv[0], asClientMasked(ws::encodeClose(1000)));
        auto bytes = recvBytes(sv[0], 500, 2);
        for (int i=0; i<200 && !closed; ++i) usleep(5000);

        r.stop(); th.join(); close(sv[0]);
        CHECK(bytes.size() >= 2);
        if (bytes.size() >= 2){
            CHECK(bytes[0] == 0x88);
            CHECK(bytes[1] == 0x00);
        }
        CHECK(closed.load());
    }

    // T5: BINARY → 关闭连接
    {
        int sv[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        Reactor r;
        std::atomic<bool> closed{false};
        WsConnection conn(sv[1], r, "alice");
        conn.setCloseHandler([&](WsConnection*){ closed = true; });
        conn.start();
        std::thread th([&]{ r.loop(); });
        usleep(30000);

        uint8_t wire[] = {0x82, 0x83, 0x01,0x02,0x03,0x04,
                          0xFF^0x01, 0xEE^0x02, 0xDD^0x03};
        sendAll(sv[0], std::vector<uint8_t>(wire, wire+sizeof(wire)));
        for (int i=0; i<200 && !closed; ++i) usleep(5000);

        r.stop(); th.join(); close(sv[0]);
        CHECK(closed.load());
    }

    printf("\nws_connection: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
// 用 socketpair(AF_UNIX) 真链路测 HttpConnection 的 onRead / onWrite / close 生命周期
#include "../../src/server/http_connection.h"
#include "../../src/server/http_request.h"
#include "../../src/server/http_response.h"
#include "../../src/server/reactor.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <thread>
#include <chrono>
#include <cerrno>

using namespace chat::server;

static std::string recvAll(int fd, int timeoutMs=300){
    char buf[4096];
    std::string out;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline){
        ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) out.append(buf, n);
        else if (n == 0) break;          // peer closed
        else if (errno == EAGAIN || errno == EWOULDBLOCK){
            usleep(2000);
        } else break;
    }
    return out;
}

static void sendAll(int fd, const std::string& s){
    ::send(fd, s.data(), s.size(), MSG_NOSIGNAL);
}

int main(){
    // 用例 1: GET 请求 → 端到端走 reactor → 拿到完整响应
    {
        int sv[2];
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        // sv[0] = client 端（test 用），sv[1] = server 端（喂给 HttpConnection）
        Reactor r;
        std::string got_path;
        HttpConnection conn(sv[1], r, [&](const HttpRequest& req, HttpConnection& c){
            got_path = req.path();
            HttpResponse resp(200);
            resp.body("hi " + req.path());
            c.send(resp);
        });
        conn.start();

        // 启动 reactor 线程
        std::thread th([&]{ r.loop(); });
        // 给 reactor 时间注册 fd
        usleep(20000);

        // client 发请求
        sendAll(sv[0], "GET /abc HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        std::string resp = recvAll(sv[0]);
        r.stop();
        th.join();

        assert(got_path == "/abc");
        assert(resp.find("HTTP/1.1 200") != std::string::npos);
        assert(resp.find("hi /abc") != std::string::npos);
        ::close(sv[0]);
        // sv[1] 已由 HttpConnection 关闭
    }

    // 用例 2: keep-alive —— 同一连接两个请求
    {
        int sv[2];
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        Reactor r;
        int count = 0;
        HttpConnection conn(sv[1], r, [&](const HttpRequest&, HttpConnection& c){
            ++count;
            c.send(HttpResponse(200).body("n=" + std::to_string(count)));
        });
        conn.start();

        std::thread th([&]{ r.loop(); });
        usleep(20000);

        sendAll(sv[0],
            "GET /a HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"
            "GET /b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        std::string resp = recvAll(sv[0], 800);
        r.stop();
        th.join();

        assert(count == 2);
        assert(resp.find("n=1") != std::string::npos);
        assert(resp.find("n=2") != std::string::npos);
        ::close(sv[0]);
    }

    // 用例 3: 半包 —— 一个请求跨两次 recv
    {
        int sv[2];
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        Reactor r;
        int count = 0;
        HttpConnection conn(sv[1], r, [&](const HttpRequest&, HttpConnection& c){
            ++count;
            c.send(HttpResponse(200).body("ok"));
        });
        conn.start();

        std::thread th([&]{ r.loop(); });
        usleep(20000);

        sendAll(sv[0], "GET /x HTTP/1.1\r\nHost: ");
        // 等 100ms：handler 还没触发
        std::string r1 = recvAll(sv[0], 100);
        assert(count == 0);
        assert(r1.empty());

        // 续上剩余
        sendAll(sv[0], "y\r\n\r\n");
        std::string r2 = recvAll(sv[0], 500);
        r.stop();
        th.join();

        assert(count == 1);
        assert(r2.find("ok") != std::string::npos);
        ::close(sv[0]);
    }

    // 用例 4: peer 关闭 → HttpConnection 调 close → sv[1] 不再可读
    {
        int sv[2];
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        Reactor r;
        HttpConnection conn(sv[1], r, [](const HttpRequest&, HttpConnection&){
            // no-op
        });
        conn.start();

        std::thread th([&]{ r.loop(); });
        usleep(20000);

        ::close(sv[0]);   // peer 关

        // 等 reactor 处理 EPOLLHUP，HttpConnection 调 close
        usleep(100000);

        // 验证 sv[1] 已被关闭
        char tmp;
        ssize_t n = ::recv(sv[1], &tmp, 1, MSG_DONTWAIT);
        // 如果 HttpConnection 关了 sv[1]，再 recv sv[1] 是 EBADF
        assert(n < 0 && errno == EBADF);

        r.stop();
        th.join();
    }

    std::cout << "test_http_connection: ALL PASS\n";
    return 0;
}
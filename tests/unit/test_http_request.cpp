#include "../../src/server/http_request.h"
#include "../../src/common/buffer.h"
#include <cassert>
#include <iostream>

using chat::common::Buffer;
using namespace chat::server;

int main(){
    // 用例 1: 简单 GET + query
    {
        Buffer b;
        b.append("GET /hello?x=1&y=2 HTTP/1.1\r\nHost: localhost\r\n\r\n");
        HttpRequest req;
        bool ok = req.parse(b);
        assert(ok && req.ok());
        assert(req.method() == "GET");
        assert(req.path() == "/hello");
        assert(req.query() == "x=1&y=2");
        assert(req.getQuery("x") == "1");
        assert(req.getQuery("y") == "2");
        assert(req.getQuery("z") == "");
        assert(req.getHeader("Host") == "localhost");
        assert(req.body().empty());
    }

    // 用例 2: POST with JSON body
    {
        Buffer b;
        std::string body = "{\"username\":\"alice\"}";
        std::string raw = "POST /api/login HTTP/1.1\r\n"
                          "Content-Length: " + std::to_string(body.size()) + "\r\n"
                          "Content-Type: application/json\r\n\r\n" + body;
        b.append(raw);
        HttpRequest req;
        assert(req.parse(b));
        assert(req.method() == "POST");
        assert(req.path() == "/api/login");
        assert(req.body() == body);
        assert(req.getHeader("Content-Type") == "application/json");
    }

    // 用例 3: header 半包（数据不够）
    {
        Buffer b;
        b.append("GET /p HTTP/1.1\r\nHost: lo");
        HttpRequest req;
        assert(!req.parse(b));   // 不够，等下次
        b.append("calhost\r\n\r\n");
        assert(req.parse(b));
        assert(req.ok());
        assert(req.path() == "/p");
        assert(req.getHeader("Host") == "localhost");
    }

    // 用例 4: body 半包
    {
        Buffer b;
        std::string body = "12345";
        b.append("POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\n12");
        HttpRequest req;
        assert(!req.parse(b));
        b.append("345");
        assert(req.parse(b));
        assert(req.body() == "12345");
    }

    // 用例 5: header 大小写不敏感
    {
        Buffer b;
        b.append("GET / HTTP/1.1\r\ncontent-TYPE: text/plain\r\n\r\n");
        HttpRequest req;
        assert(req.parse(b));
        assert(req.getHeader("Content-Type") == "text/plain");
        assert(req.getHeader("content-type") == "text/plain");
    }

    // 用例 6: 版本号校验（HTTP/1.0 也接受）
    {
        Buffer b;
        b.append("GET / HTTP/1.0\r\n\r\n");
        HttpRequest req;
        assert(req.parse(b));
        assert(req.version() == "HTTP/1.0");
    }

    // 用例 7: 协议错（找不到版本）
    {
        Buffer b;
        b.append("GARBAGE no crlf");
        HttpRequest req;
        assert(!req.parse(b));
    }

    // 用例 8: pipeline —— 一个 buffer 里两个请求
    {
        Buffer b;
        std::string raw =
            "GET /a HTTP/1.1\r\n\r\n"
            "GET /b HTTP/1.1\r\n\r\n";
        b.append(raw);
        HttpRequest r1;
        assert(r1.parse(b));
        assert(r1.path() == "/a");
        // 剩余字节（第二个请求）应回到 buf
        HttpRequest r2;
        assert(r2.parse(b));
        assert(r2.path() == "/b");
    }

    std::cout << "test_http_request: ALL PASS\n";
    return 0;
}

#include "../../src/server/http_response.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace chat::server;

static bool contains(const std::string& s, const std::string& sub){
    return s.find(sub) != std::string::npos;
}

int main(){
    // 用例 1: 默认 200 OK + 自动 Content-Length
    {
        HttpResponse r;
        r.body("hello");
        std::string s = r.toString();
        assert(contains(s, "HTTP/1.1 200 OK\r\n"));
        assert(contains(s, "Content-Length: 5\r\n"));
        assert(contains(s, "Connection: close\r\n"));
        assert(contains(s, "\r\n\r\nhello"));
        assert(!r.keepAlive());
    }

    // 用例 2: 链式构造 + 改 status
    {
        HttpResponse r;
        r.status(404).header("X-Trace", "abc").body("not found");
        std::string s = r.toString();
        assert(contains(s, "HTTP/1.1 404 Not Found\r\n"));
        assert(contains(s, "X-Trace") && contains(s, "abc\r\n"));
        assert(contains(s, "Content-Length: 9\r\n"));
        assert(contains(s, "\r\n\r\nnot found"));
    }

    // 用例 3: bodyJson 自动设 Content-Type
    {
        HttpResponse r;
        r.bodyJson("{\"ok\":true}");
        std::string s = r.toString();
        assert(contains(s, "Content-Type") && contains(s, "application/json"));
        assert(contains(s, "\r\n\r\n{\"ok\":true}"));
    }

    // 用例 4: keepAlive
    {
        HttpResponse r(204);
        r.setKeepAlive(true);
        std::string s = r.toString();
        assert(contains(s, "Connection: keep-alive\r\n"));
        assert(contains(s, "Content-Length: 0\r\n"));
    }

    // 用例 5: 自定义 reason
    {
        HttpResponse r;
        r.status(418, "I'm a teapot");
        std::string s = r.toString();
        assert(contains(s, "HTTP/1.1 418 I'm a teapot\r\n"));
    }

    // 用例 6: 用户已设 Content-Length，不重复加
    {
        HttpResponse r;
        r.header("Content-Length", "999").body("abc");
        std::string s = r.toString();
        auto pos = s.find("Content-Length:");
        assert(pos != std::string::npos);
        assert(s.find("999", pos) != std::string::npos);
        auto pos2 = s.find("Content-Length:", pos + 1);
        assert(pos2 == std::string::npos);
    }

    // 用例 7: makeError
    {
        HttpResponse r = HttpResponse::makeError(400, "bad");
        std::string s = r.toString();
        assert(contains(s, "HTTP/1.1 400 Bad Request\r\n"));
        assert(contains(s, "{\"error\":\"bad\"}"));
    }

    // 用例 8: makeJson
    {
        HttpResponse r = HttpResponse::makeJson(201, "{\"id\":1}");
        std::string s = r.toString();
        assert(contains(s, "HTTP/1.1 201 Created\r\n"));
        assert(contains(s, "application/json"));
    }

    std::cout << "test_http_response: ALL PASS\n";
    return 0;
}

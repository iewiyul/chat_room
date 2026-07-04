#pragma once

#include "reactor.h"
#include "http_connection.h"
#include "http_handler.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <functional>

namespace chat::server{
    class HttpServer{
    public:
        // 阶段 3 改造：ctor 多收 HttpDeps*（存指针,生命周期由 main.cpp 管理）
        HttpServer(Reactor& r, uint16_t port, std::string webroot,
                   const HttpDeps* deps);
        ~HttpServer();

        bool start();

        using RouteHandler=std::function<void(const HttpRequest&,HttpConnection&)>;
        void addRoute(const std::string& pathPrefix, RouteHandler h);

        void stop();

    private:
        // 接收新连接
        void onAccept_();
        // 路由分发
        void dispatch_(const HttpRequest& req, HttpConnection& conn);
        // 阶段 3：处理 WS upgrade
        void handleWsUpgrade_(const HttpRequest& req, HttpConnection& conn);

        // 静态：MIME 表
        static std::string mimeType(const std::string& path);

        Reactor&        reactor_;
        uint16_t        port_;
        std::string     webroot_;
        int             listenFd_ = -1;
        const HttpDeps* deps_ = nullptr;   // 阶段 3：用于 WS upgrade 时拿 hub/reactor/tokens

        std::unordered_map<std::string, RouteHandler> routes_;
        // HttpConnection 自己持 fd + 在 reactor 注册回调，
        // 所以 Server 必须持有所有权（否则 onAccept_ 返回后 unique_ptr
        // 出作用域、连接被销毁，下次 reactor 触发回调就是 use-after-free）。
        std::unordered_map<int, std::unique_ptr<HttpConnection>> conns_;
    };
}
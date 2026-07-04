#pragma once

#include "../common/buffer.h"
#include "http_request.h"
#include "http_response.h"
#include <functional>
#include "reactor.h"

namespace chat::server{
    class HttpConnection;  // 前向声明，供 HttpHandler 别名使用
    using HttpHandler=std::function<void(const HttpRequest&,HttpConnection&)>;
    using chat::common::Buffer;

    class HttpConnection{
    public:
        using CloseCallback = std::function<void(HttpConnection*)>;

        explicit HttpConnection(int fd,Reactor& reactor,HttpHandler handler);

        void start();
        //把序列化后的字节流发送出去
        void send(const HttpResponse& resp);

        void setCloseCallback(CloseCallback cb){onClose_=std::move(cb);}

        Buffer& writeBuf(){return writeBuf_;}

        bool closeAfterWrite() const {return closeAfterWrite_;}
        int fd() const {return fd_;}

        // ===== 阶段 3：WS upgrade 移交 =====
        // 在 send(101) 之前调用，标记此连接即将升级。
        // close() 在 upgraded_=true 时不会 ::close(fd_)，让 fd 安全转交 WsConnection。
        // username 用于 close 回调时交给 WsHub 建立 ws 用户上下文。
        void markUpgraded(std::string username);
        bool isUpgraded() const { return upgraded_; }
        const std::string& upgradedUser() const { return upgradedUser_; }

    private:
        enum State{ACTIVE, CLOSED};
        int fd_;
        Reactor& reactor_;
        HttpHandler onRequest_;
        CloseCallback onClose_;
        Buffer readBuf_;
        Buffer writeBuf_;
        bool closeAfterWrite_=false;
        bool writing_=false;
        State state_=ACTIVE;
        bool upgraded_=false;
        std::string upgradedUser_;

        //客户端读写回调
        void onRead();
        void onWrite();
        //关闭回调
        void close();
    };
}
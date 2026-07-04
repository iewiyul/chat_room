#pragma once

#include "../common/buffer.h"
#include "ws_frame.h"
#include "reactor.h"

#include <functional>
#include <string>
#include <cstdint>

namespace chat::server{
    class WsConnection;   // 前向声明

    // 收到文本帧时回调（payload 是 UTF-8 字节流,通常是个 JSON 字符串）
    using WsTextHandler    = std::function<void(WsConnection*, const std::string&)>;
    // 连接关闭时回调（不论原因）
    using WsCloseHandler   = std::function<void(WsConnection*)>;

    using chat::common::Buffer;

    class WsConnection{
    public:
        enum State { ACTIVE, CLOSED };

        // 构造：fd 由外部创建（HttpConnection upgrade 移交 或 accept 直接得到）
        WsConnection(int fd, Reactor& reactor, std::string username);

        // 注册回调
        void setTextHandler(WsTextHandler cb){ onText_ = std::move(cb); }
        void setCloseHandler(WsCloseHandler cb){ onClose_ = std::move(cb); }

        // 启动 epoll 监听
        void start();

        // 发送文本帧（编码 → writeBuf → modifyFd(EPOLLIN|EPOLLOUT)）
        void sendText(const std::string& payload);
        // 主动发 ping
        void sendPing(const std::string& payload = "");
        // 主动关闭（发 close 帧 → 等对方 ack → 真正 close;demo 简化直接 close）
        void close();

        // 上下文
        int                fd()    const { return fd_; }
        const std::string& username() const { return username_; }
        bool               isActive() const { return state_ == ACTIVE; }

    private:
        int           fd_;
        Reactor&      reactor_;
        std::string   username_;     // 来自 token 验证

        Buffer        readBuf_;
        Buffer        writeBuf_;
        bool          writing_       = false;
        State         state_         = ACTIVE;
        bool          closeAfterWrite_ = false;  // 收到/发了 close 帧后置 true

        WsTextHandler  onText_;
        WsCloseHandler onClose_;

        // reactor 回调
        void onRead();
        void onWrite();
    };
}
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>
#include "message.h"

namespace chat::server{
    class Room{
    public:
        static constexpr size_t MAX_HISTORY=200;

        // 消息存储 + 订阅回调
        void addMessage(const Message& m);
        std::vector<Message> recentMessages(int64_t since) const;

        // 注册单个 onMessage 回调（addMessage 触发，锁外调用）
        // 阶段 3 引入：WsHub 注册自己，实现"HTTP/TCP 发消息 → 实时推到所有 WS 客户端"
        using MessageCallback = std::function<void(const Message&)>;
        void setOnMessage(MessageCallback cb){ onMessage_ = std::move(cb); }

        //用户集
        void addUser(const std::string& name);
        void removeUser(const std::string& name);
        std::vector<std::string> users() const;
        bool hasUser(const std::string& name) const;

        //历史条数
        size_t messageCount() const;

    private:
        mutable std::mutex mu_;
        std::deque<Message> messages_;
        std::unordered_set<std::string> users_;
        MessageCallback onMessage_;   // 单订阅者；多订阅可换成 vector
    };
}
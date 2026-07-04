#pragma once

#include <cstdint>
#include <string>
#include "../third_party/nlohmann/json.hpp"

namespace chat::server{
    // 存进 Room 的聊天消息
    // （注意：不要和 chat::protocol::MessageType 同名 —— 那个是线缆协议层）
    struct Message{
        std::string from;
        std::string room;
        std::string content;
        int64_t     time;

        nlohmann::json toJson() const;
    };
}
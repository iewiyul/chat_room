#pragma once

#include <cstdint>
#include <iostream>

namespace chat::protocol{
    //消息类型
    enum MessageType:uint8_t{
        MSG_LOGIN        = 1,
        MSG_LOGIN_ACK    = 2,
        MSG_CHAT         = 3,
        MSG_CHAT_BROAD   = 4,
        MSG_USER_LIST    = 5,
        MSG_PRIVATE      = 6,
        MSG_PING         = 7,
        MSG_PONG         = 8,
        MSG_LOGOUT       = 9,
        MSG_SYSTEM       = 10,
    };
    
    //协议头长度
    constexpr size_t HEADER_SIZE=4;
    constexpr size_t TYPE_SIZE=1;
    constexpr size_t MIN_PACKAGE_SIZE=HEADER_SIZE+TYPE_SIZE;
    constexpr size_t MAX_BODY_LENGTH=64*1024;

    //字段常量文件
    namespace field {
        constexpr const char* USERNAME = "username";
        constexpr const char* FROM     = "from";
        constexpr const char* TO       = "to";
        constexpr const char* ROOM     = "room";
        constexpr const char* CONTENT  = "content";
        constexpr const char* TIME     = "time";
        constexpr const char* USERS    = "users";
        constexpr const char* OK       = "ok";
        constexpr const char* ERROR    = "error";
    }
}

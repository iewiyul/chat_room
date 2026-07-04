#pragma once
#include "connection.h"
#include "user_manager.h"
#include "../protocol/packet.h"
#include <string>

namespace chat::server{
    class MessageHandler{
    public:
        MessageHandler(UserManager& users):users_(users){}

        //包回调逻辑
        void onPacket(Connection* conn,const chat::protocol::Packet& pkt);

        //对端断开或是超时（由 Connection::setCloseCallback 注册）
        void onClose(Connection* conn);

    private:
        void handleLogin(Connection* conn,const chat::protocol::Packet& pkt);
        void handleChat(Connection* conn,const chat::protocol::Packet& pkt);
        void handlePrivate(Connection* conn,const chat::protocol::Packet& pkt);
        void handleUserList(Connection* conn);
        void handlePing(Connection* conn);
        void handleLogOut(Connection* conn);

        void sendAck(Connection* conn,bool ok,const std::string& err="");
        void broadcastSystem(const std::string& text);
        void broadcastUserList();

        UserManager& users_;
    };
}

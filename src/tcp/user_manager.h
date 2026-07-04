#pragma once
#include "connection.h"
#include "../protocol/packet.h"
#include <unordered_map>
#include <vector>
#include <string>

namespace chat::server{
    class UserManager{
    public:
        //添加用户
        bool add(const std::string& username,Connection* conn);
        //移除用户
        void remove(const std::string& username);
        void remove(Connection* conn);
        //获得需要连接的对象用户
        Connection* get(const std::string& username);
        std::vector<std::string> usernames();
        //广播
        void broadcast(const chat::protocol::Packet& pkt);
        void broadcast(const chat::protocol::Packet& pkt,Connection* except);
        size_t size() const{return users_.size();}

    private:
        std::unordered_map<std::string,Connection*> users_;
    };
}

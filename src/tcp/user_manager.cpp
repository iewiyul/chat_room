#include "user_manager.h"
#include "../protocol/packet.h"

namespace chat::server{
    bool UserManager::add(const std::string& username,Connection* conn){
        if(users_.count(username)){return false;}
        users_[username]=conn;
        return true;
    }

    void UserManager::remove(const std::string& username){
        users_.erase(username);
    }

    void UserManager::remove(Connection* conn){
        for(auto it=users_.begin();it!=users_.end();it++){
            if(it->second==conn){users_.erase(it->first);break;}
        }
    }

    Connection* UserManager::get(const std::string& username){
        auto it=users_.find(username);
        return it==users_.end()?nullptr:it->second;
    }

    std::vector<std::string> UserManager::usernames(){
        std::vector<std::string> res;
        for(auto it=users_.begin();it!=users_.end();it++){
            res.push_back(it->first);
        }
        return res;
    }

    void UserManager::broadcast(const chat::protocol::Packet& pkt){
        for(auto& kv:users_){
            if(kv.second){kv.second->send(pkt);}
        }
    }

    void UserManager::broadcast(const chat::protocol::Packet& pkt,Connection* except){
        for(auto& kv:users_){
            if(kv.second && kv.second!=except){kv.second->send(pkt);}
        }
    }
}
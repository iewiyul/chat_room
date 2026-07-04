#include "room.h"

namespace chat::server{
    void Room::addMessage(const Message& m){
        {
            std::lock_guard<std::mutex> lk(mu_);
            messages_.push_back(m);
            if(messages_.size()>MAX_HISTORY){
                messages_.pop_front();
            }
        }
        // 锁外回调：避免订阅者在回调里再调 Room 方法时死锁
        // （订阅者 WsHub.broadcast 仅 sendText，不会回 Room，安全）
        if (onMessage_) onMessage_(m);
    }

    std::vector<Message> Room::recentMessages(int64_t since) const{
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Message> out;
        for(auto& m:messages_){
            if(m.time>since){out.push_back(m);}
        }
        return out;
    }

    void Room::addUser(const std::string& name){
        std::lock_guard<std::mutex> lk(mu_);
        users_.insert(name);
    }

    void Room::removeUser(const std::string& name){
        std::lock_guard<std::mutex> lk(mu_);
        users_.erase(name);
    }

    std::vector<std::string> Room::users() const{
        std::lock_guard<std::mutex> lk(mu_);
        return std::vector<std::string>(users_.begin(),users_.end());
    }

    bool Room::hasUser(const std::string& name) const{
        std::lock_guard<std::mutex> lk(mu_);
        return users_.count(name)>0;
    }

    size_t Room::messageCount() const{
        std::lock_guard<std::mutex> lk(mu_);
        return messages_.size();
    }
}
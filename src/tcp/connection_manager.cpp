#include "connection_manager.h"
#include "connection.h"
#include <vector>

namespace chat::server{
    void ConnectionManager::add(Connection* c){conns_.insert(c);}
    void ConnectionManager::remove(Connection* c){conns_.erase(c);}

    void ConnectionManager::tick(int timeoutSec){
        time_t now=::time(nullptr);
        // ⚠️ 不能在迭代 conns_ 的同时 close：c->close() 触发 onClose_
        //    → connMgr.remove(c) → erase 当前元素，迭代器失效
        std::vector<Connection*> toClose;
        for(auto* c:conns_){
            if(now-c->lastActiveTime()>timeoutSec){
                toClose.push_back(c);
            }
        }
        for(auto* c:toClose){
            c->close();
        }
    }
}

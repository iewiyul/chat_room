#pragma once
#include "connection.h"
#include <unordered_set>

namespace chat::server{
    class ConnectionManager{
    public:
        void add(Connection* c);
        void remove(Connection* c);
        void tick(int timeoutSec=30);

    private:
        std::unordered_set<Connection*> conns_;
    };
}

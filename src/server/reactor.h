#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <sys/epoll.h>
#include <cstring>
#include <stdexcept>

namespace chat::server{
    class Reactor{
    public:
        using EventCallback=std::function<void()>;
        using TickCallback=std::function<void()>;

        Reactor();
        ~Reactor();

        void setTickCallback(TickCallback& cb){tickCb_=std::move(cb);}

        //注册fd+读写回调
        void addFd(int fd,uint32_t events,EventCallback onRead,EventCallback onWrite);

        //修改关注的事件
        void modifyFd(int fd,uint32_t events);

        //删除fd
        void removeFd(int fd);

        //主循环
        void loop();

        //跨线程调用安全
        void stop(){
            running_=false;
        }

    private:
        struct FdContext{
            EventCallback onRead;
            EventCallback onWrite;
        };

        int epollFd_;
        bool running_;
        std::unordered_map<int,FdContext> contexts_;
        TickCallback tickCb_;

        void handleEvent(int fd,uint32_t events);
    };
}
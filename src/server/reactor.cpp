#include "reactor.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace chat::server{
    Reactor::Reactor():epollFd_(::epoll_create1(EPOLL_CLOEXEC)),running_(true){
        if(epollFd_<0){
            throw std::runtime_error("epoll_create1 failed: "+std::string(strerror(errno)));
        }
    }

    Reactor::~Reactor(){
        if(epollFd_>=0){::close(epollFd_);}
    }

    void Reactor::addFd(int fd,uint32_t events,EventCallback onRead,EventCallback onWrite){
        epoll_event ev{};
        ev.events=events;
        ev.data.fd=fd;
        if(::epoll_ctl(epollFd_,EPOLL_CTL_ADD,fd,&ev)<0){
            throw std::runtime_error("epoll_ctl ADD failed");
        }
        contexts_[fd]={std::move(onRead),std::move(onWrite)};
    }

    void Reactor::modifyFd(int fd,uint32_t events){
        epoll_event ev{};
        ev.events=events;
        ev.data.fd=fd;
        ::epoll_ctl(epollFd_,EPOLL_CTL_MOD,fd,&ev);
    }

    void Reactor::removeFd(int fd){
        ::epoll_ctl(epollFd_,EPOLL_CTL_DEL,fd,nullptr);
        contexts_.erase(fd);
    }

    void Reactor::loop(){
        epoll_event events[1024];

        while(running_){
            int n=::epoll_wait(epollFd_,events,1024,1000);
            if(n<0){
                if(errno==EINTR){continue;}
                break;
            }

            if(0==n && tickCb_){
                tickCb_();
            }

            for(int i=0;i<n;i++){
                int fd=events[i].data.fd;
                uint32_t ev=events[i].events;
                handleEvent(fd,ev);
            }
        }
    }

    void Reactor::handleEvent(int fd,uint32_t events){
        auto it=contexts_.find(fd);
        if(it==contexts_.end()){return;}

        auto readCb=it->second.onRead;
        auto writeCb=it->second.onWrite;

        if(events & (EPOLLERR | EPOLLHUP)){
            if(readCb){readCb();}
            return;
        }

        if(events & EPOLLIN && readCb){readCb();}
        if(events & EPOLLOUT && writeCb){writeCb();}
    }
}
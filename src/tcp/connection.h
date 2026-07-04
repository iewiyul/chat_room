#pragma once
#include "../common/buffer.h"
#include "../protocol/packet.h"

#include <cstdint>
#include <functional>
#include <string>
#include <sys/epoll.h>

namespace chat::server{
    class Reactor;

    class Connection{
    public:
        enum State{CONNECTING,AUTHENTICATED,CLOSED};

        using PacketCallback=std::function<void(Connection*,const chat::protocol::Packet&)>;
        using CloseCallback=std::function<void(Connection*)>;

        Connection(int fd,Reactor& reactor);
        ~Connection();

        //注册fd到reactor
        void start();

        //清理fd
        void close();

        //发送
        void send(const chat::protocol::Packet& pkt);

        //包处理回调
        void setPacketCallback(PacketCallback cb){onPacket_=std::move(cb);}

        //关闭处理回调
        void setCloseCallback(CloseCallback cb){onClose_=std::move(cb);}

        //访问器
        int fd() const{return fd_;}
        State state(){return state_;}
        const std::string& username() const{return username_;}
        void setUsername(const std::string& name){username_=name;state_=AUTHENTICATED;}
        time_t lastActiveTime() const{return lastActive_;}
        void touch(){lastActive_=::time(nullptr);}

    private:
        int fd_;
        Reactor& reactor_;
        chat::common::Buffer readBuf_;
        chat::common::Buffer writeBuf_;
        State state_=CONNECTING;
        std::string username_;
        PacketCallback onPacket_;
        CloseCallback onClose_;
        bool writing_=false;
        time_t lastActive_=0;

        //reactor回调
        void onRead();
        void onWrite();

        //收到一个完整的包后的处理
        void handlePacket(const chat::protocol::Packet& pkt);
    };
}
#include "connection.h"
#include "../server/reactor.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../protocol/packet.h"

namespace chat::server{
    Connection::Connection(int fd,Reactor& reactor):fd_(fd),reactor_(reactor){
        touch();
    }
    Connection::~Connection(){
        if(CLOSED!=state_){close();}
    }

    void Connection::start(){
        reactor_.addFd(fd_,EPOLLIN,[this]{onRead();},[this]{onWrite();});
    }

    void Connection::onRead(){
        while(true){
            //把数据塞进readBuf
            ssize_t n=::recv(fd_,readBuf_.writeBegin(),readBuf_.writableBytes(),0);

            if(n>0){
                readBuf_.hasWritten(n);
                touch();
            }else if(0==n){
                close();
                return;
            }else{
                if(errno==EAGAIN || errno==EWOULDBLOCK){break;}
                close();
                return;
            }
        }

        //循环解码所有完整包
        while(readBuf_.readableBytes()>0){
            auto r=chat::protocol::decode(readBuf_.peek(),readBuf_.readableBytes());

            if(!r.ok){
                if(0==r.consumed){close();}
                return;
            }
            readBuf_.consume(r.consumed);
            const chat::protocol::Packet& pkt=r.packet;
            handlePacket(pkt);
        }
    }

    void Connection::handlePacket(const chat::protocol::Packet& pkt){
        if(onPacket_){
            onPacket_(this,pkt);
        }
    }

    //只是把数据放入写缓冲区
    void Connection::send(const chat::protocol::Packet& pkt){
        auto bytes=chat::protocol::encode(pkt);

        writeBuf_.append(bytes.data(),bytes.size());

        if(!writing_){
            reactor_.modifyFd(fd_,EPOLLIN | EPOLLOUT);
            writing_=true;
        }
    }

    void Connection::onWrite(){
        while(writeBuf_.readableBytes()>0){
            ssize_t n=::send(fd_,writeBuf_.peek(),writeBuf_.readableBytes(),MSG_NOSIGNAL);

            if(n>0){
                writeBuf_.consume(n);
                touch();
            }else if(n<0){
                if(errno==EAGAIN || errno==EWOULDBLOCK){return;}
                close();
                return;
            }else{
                close();
                return;
            }
        }

        if(writing_){
            reactor_.modifyFd(fd_,EPOLLIN);
            writing_=false;
        }
    }

    void Connection::close(){
        if(state_==CLOSED){return;}
        
        reactor_.removeFd(fd_);
        if(onClose_){onClose_(this);}
        state_=CLOSED;
        ::close(fd_);
    }
}
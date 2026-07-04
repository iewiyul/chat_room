#include "http_connection.h"
#include "reactor.h"
#include <sys/socket.h>
#include <unistd.h>

namespace chat::server{
    HttpConnection::HttpConnection(int fd,Reactor& reactor,HttpHandler handler):
    fd_(fd),reactor_(reactor),onRequest_(std::move(handler)){}

    void HttpConnection::start(){
        reactor_.addFd(fd_,EPOLLIN,[this](){onRead();},[this](){onWrite();});
    }

    void HttpConnection::onRead(){
        char tmp[4096];
        while(true){
            ssize_t n=::recv(fd_,tmp,sizeof(tmp),MSG_DONTWAIT);
            if(n>0){
                readBuf_.append(reinterpret_cast<const uint8_t*>(tmp),static_cast<size_t>(n));
            }else if(n==0){
                close();
                return;
            }else{
                if(errno == EAGAIN || errno == EWOULDBLOCK){break;}
                close();
                return;
            }
        }

        //循环解码http请求
        while(true){
            HttpRequest req;
            if(!req.parse(readBuf_)){return;}
            onRequest_(req,*this);
        }
    }

    void HttpConnection::onWrite(){
        while(writeBuf_.readableBytes()>0){
            ssize_t n=::send(fd_,writeBuf_.peek(),writeBuf_.readableBytes(),MSG_NOSIGNAL | MSG_DONTWAIT);
            if(n>0){
                writeBuf_.consume(static_cast<size_t>(n));
            }else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)){
                return;
            }else{
                close();
                return;
            }
        }

        if(0==writeBuf_.readableBytes()){
            if(closeAfterWrite()){
                close();
            }else{
                reactor_.modifyFd(fd_,EPOLLIN);
            }
        }
    }

    void HttpConnection::send(const HttpResponse& resp){
        std::string s=resp.toString();
        writeBuf_.append(s);
        if(!resp.keepAlive()){closeAfterWrite_=true;}

        if(!writing_){
            reactor_.modifyFd(fd_,EPOLLIN | EPOLLOUT);
            writing_=true;
        }
    }

    void HttpConnection::close(){
        if(state_==CLOSED){return;}
        state_=CLOSED;
        reactor_.removeFd(fd_);
        if(onClose_){onClose_(this);}
        if(!upgraded_ && fd_>=0){ ::close(fd_); }
    }

    void HttpConnection::markUpgraded(std::string username){
        upgraded_=true;
        upgradedUser_=std::move(username);
    }
}
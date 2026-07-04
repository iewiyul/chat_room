#include "reactor.h"
#include "connection.h"
#include "user_manager.h"
#include "message_handler.h"
#include "connection_manager.h"
#include "../protocol/packet.h"
#include "../common/utils.h"

#include <iostream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace chat::server;
using chat::protocol::Packet;

static Reactor* g_reactor=nullptr;

static void onSigInt(int){if(g_reactor){g_reactor->stop();}}

int main(int argc,char* argv[]){
    int port=(argc>1)?atoi(argv[1]):9000;
    int http_port=(argc>2)?atoi(argv[2]):0;
    const char* webroot=(argc>3)?argv[3]:"src/frontend";

    int listenfd=::socket(AF_INET,SOCK_STREAM,0);
    int opt=1;
    ::setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    addr.sin_port=htons(port);
    if(::bind(listenfd,(sockaddr*)&addr,sizeof(addr))<0){perror("bind()");return -1;}
    if(::listen(listenfd,120)<0){perror("listen()");return -1;}
    chat::common::setNonBlocking(listenfd);

    Reactor reactor;
    g_reactor=&reactor;
    UserManager users;
    MessageHandler handler(users);
    ConnectionManager connMgr;

    Reactor::TickCallback tcb=[&]{connMgr.tick();};
    reactor.setTickCallback(tcb);

    reactor.addFd(listenfd,EPOLLIN,[&]{
        while(true){
            sockaddr_in client{};
            socklen_t len=sizeof(client);
            int fd=::accept(listenfd,(sockaddr*)&client,&len);
            if(fd<0){
                if(errno==EAGAIN || errno==EWOULDBLOCK){break;}
                perror("accept()");
                break;
            }
            chat::common::setNonBlocking(fd);

            auto* c=new Connection(fd,reactor);
            c->setCloseCallback([&](Connection* cc){
                handler.onClose(cc);
                connMgr.remove(cc);
            });
            c->setPacketCallback([&](Connection* cc,const Packet& pkt){
                handler.onPacket(cc,pkt);
            });
            c->start();
            connMgr.add(c);

            char ip[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET,&client.sin_addr,ip,sizeof(ip));
            printf("[server] + %s:%d fd=%d\n",ip,ntohs(client.sin_port),fd);
        }
    },nullptr);

    std::signal(SIGINT,onSigInt);
    std::signal(SIGTERM,onSigInt);

    reactor.loop();
    printf("%s","server bye\n");
    ::close(listenfd);
    return 0;
}
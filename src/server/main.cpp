#include "reactor.h"
#include "room.h"
#include "token_store.h"
#include "ws_hub.h"
#include "http_server.h"
#include "http_handler.h"

#include <iostream>
#include <csignal>
#include <cstdio>
#include <cstdlib>

using namespace chat::server;

static Reactor* g_reactor = nullptr;
static void onSigInt(int){ if (g_reactor){ g_reactor->stop(); } }

int main(int argc, char* argv[]){
    int http_port = (argc > 1) ? atoi(argv[1]) : 8080;
    const char* webroot = (argc > 2) ? argv[2] : "src/frontend";

    Reactor reactor;
    g_reactor = &reactor;

    // 共享业务对象
    Room       room;
    TokenStore tokens;

    // WsHub 订阅 Room:任何渠道 addMessage 自动推到所有 WS 客户端
    WsHub    wsHub(room, reactor);

    HttpDeps deps{&room, &tokens, &wsHub, &reactor};
    HttpServer http(reactor, (uint16_t)http_port, webroot, &deps);
    registerHttpRoutes(http, deps);

    if (!http.start()){
        fprintf(stderr, "[main] HTTP start failed\n");
        return 1;
    }
    fprintf(stderr, "[main] listening on :%d, webroot=%s\n", http_port, webroot);

    std::signal(SIGINT, onSigInt);
    std::signal(SIGTERM, onSigInt);

    reactor.loop();
    printf("server bye\n");
    return 0;
}
#pragma once

#include "room.h"
#include "token_store.h"
#include "http_request.h"
#include "http_response.h"
#include "http_connection.h"
#include "reactor.h"
#include "ws_hub.h"
#include <nlohmann/json.hpp>
#include <string>

namespace chat::server{

// 全部路由在这里挂
struct HttpDeps{
    Room*       room;
    TokenStore* tokens;
    WsHub*      hub;       // 阶段 3：WS upgrade 时把 fd 移交给它
    Reactor*    reactor;   // WsHub 需要 reactor 引用；这里转发
};

void registerHttpRoutes(class HttpServer& s, const HttpDeps& deps);

// 工具：把 HttpResponse 转成给 conn.send 的快捷方式
HttpResponse jsonResp(int code, const nlohmann::json& j);

}
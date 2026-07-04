#include "ws_hub.h"
#include <nlohmann/json.hpp>
#include <chrono>

namespace chat::server{

namespace{
    // 简易时间戳（与 http_handler.cpp 里的 nowMs 等价）
    int64_t nowMs(){
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
    }
}

    WsHub::WsHub(Room& room, Reactor& reactor) : room_(room), reactor_(reactor){
        // 注册到 Room：Room::addMessage 末尾触发 onRoomMessage_
        room_.setOnMessage([this](const Message& m){ onRoomMessage_(m); });
    }

    WsHub::~WsHub(){
        room_.setOnMessage(nullptr);   // 解绑回调,防止悬空
    }

    void WsHub::adoptConnection(int fd, const std::string& username){
        auto* conn = new WsConnection(fd, reactor_, username);

        // onClose: WsConnection 关掉时,从集合里 erase 并 delete
        conn->setCloseHandler([this](WsConnection* c){
            conns_.erase(c);
            delete c;
        });

        // onText: 收到文本帧 → 解析 JSON → 转给 Room
        // 期望格式:{"type":"chat","room":"lobby","content":"hello"}
        // from 自动取 conn->username()（HttpServer::handleWsUpgrade_ 时验过 token）
        conn->setTextHandler([this](WsConnection* c, const std::string& payload){
            try {
                auto j = nlohmann::json::parse(payload);
                if (j.value("type", std::string{}) != "chat") return;
                std::string content = j.value("content", std::string{});
                if (content.empty()) return;

                Message m{c->username(),
                          j.value("room", std::string{"lobby"}),
                          content,
                          nowMs()};
                room_.addMessage(m);   // 触发 WsHub::onRoomMessage_ → broadcast
            } catch (...) {
                // JSON 解析失败,忽略(不报错,默默丢)
            }
        });

        conn->start();
        conns_.insert(conn);

        // 不再回填历史:客户端通过 HTTP GET /api/messages 拉首屏历史。
        // WS 只负责推"之后"的新消息,职责单一,避免双重回填。
    }

    void WsHub::remove(WsConnection* c){
        auto it = conns_.find(c);
        if (it != conns_.end()){
            conns_.erase(it);
            delete *it;
        }
    }

    void WsHub::broadcast(const Message& m){
        auto json = toWsJson_(m);
        // 复制一份 set,防止回调里又 add/remove 导致迭代器失效
        auto snapshot = conns_;
        for (auto* c : snapshot){
            if (c->isActive()){
                c->sendText(json);
            }
        }
    }

    void WsHub::onRoomMessage_(const Message& m){
        broadcast(m);
    }

    std::string WsHub::toWsJson_(const Message& m){
        // 与 HTTP /api/messages GET 返回的 JSON 兼容：type 字段标识消息种类
        nlohmann::json j = m.toJson();
        j["type"] = "chat";
        return j.dump();
    }

}  // namespace chat::server
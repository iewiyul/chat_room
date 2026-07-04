#pragma once

#include "room.h"
#include "ws_connection.h"

#include <unordered_set>
#include <string>

namespace chat::server{

// WsHub —— 所有 WS 连接的中枢:
//   - 持有 Room 引用,启动时把 broadcast 注册到 Room::setOnMessage
//   - 维护当前所有 WsConnection*
//   - 收到 HTTP→Room::addMessage 时,自动推到所有 WS 客户端
//
// 生命周期约束:
//   - WsConnection 的所有权在 WsHub（WsHub::add 加入集合）
//   - 关闭走 setCloseHandler,WsHub 在回调里 erase 自己
//   - addConnection / adoptConnection 必须在 reactor 线程里调用
class WsHub{
public:
    WsHub(Room& room, Reactor& reactor);
    ~WsHub();

    // HttpServer upgrade 完成时调用：拿 fd+username 创建 WsConnection,
    // 启动,加入集合,回填历史消息。
    void adoptConnection(int fd, const std::string& username);

    // 主动移除（demo 暂未用）
    void remove(WsConnection* c);

    // 广播一条消息（通常是 Room::onMessage 调过来）
    void broadcast(const Message& m);

    // 当前在线 WS 连接数
    size_t size() const { return conns_.size(); }

private:
    Room&    room_;
    Reactor& reactor_;
    std::unordered_set<WsConnection*> conns_;   // reactor 单线程,无锁

    // 注册到 Room 的回调
    void onRoomMessage_(const Message& m);

    // 拼装 ws payload 的 JSON
    static std::string toWsJson_(const Message& m);
};

}  // namespace chat::server
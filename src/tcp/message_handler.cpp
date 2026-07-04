#include "message_handler.h"
#include "../protocol/message.h"
#include "../third_party/nlohmann/json.hpp"

namespace chat::server{
    using nlohmann::json;
    namespace field=chat::protocol::field;

    void MessageHandler::onPacket(Connection* conn,const chat::protocol::Packet& pkt){
        switch(pkt.type){
            case chat::protocol::MSG_LOGIN:     handleLogin(conn,pkt);     break;
            case chat::protocol::MSG_CHAT:      handleChat(conn,pkt);      break;
            case chat::protocol::MSG_PRIVATE:   handlePrivate(conn,pkt);   break;
            case chat::protocol::MSG_USER_LIST: handleUserList(conn);      break;
            case chat::protocol::MSG_PING:      handlePing(conn);          break;
            case chat::protocol::MSG_LOGOUT:    handleLogOut(conn);        break;
            default: break;  
        }
    }

    void MessageHandler::handleLogin(Connection* conn,const chat::protocol::Packet& pkt){
        if(conn->state()==Connection::AUTHENTICATED){
            sendAck(conn,false,"已登录");
            return;
        }

        json j;
        try{j=json::parse(pkt.body);}
        catch(...){sendAck(conn,false,"消息格式错误");return;}

        std::string username=j.value(field::USERNAME,"");
        if(username.empty()){
            sendAck(conn,false,"用户名不能为空");
            return;
        }

        if(!users_.add(username,conn)){
            sendAck(conn,false,"用户名已存在");
            return;
        }

        conn->setUsername(username);
        sendAck(conn,true);

        broadcastSystem(username+"上线");
        broadcastUserList();
    }

    void MessageHandler::handleChat(Connection* conn,const chat::protocol::Packet& pkt){
        if(Connection::AUTHENTICATED!=conn->state()){
            sendAck(conn,false,"未登录");
            return;
        }

        json j;
        try{j=json::parse(pkt.body);}
        catch(...){return;}

        std::string content=j.value(field::CONTENT,"");
        if(content.empty()){return;}

        json out;
        out[field::FROM]=conn->username();
        out[field::CONTENT]=content;
        out[field::TIME]=time(nullptr);

        chat::protocol::Packet bp{chat::protocol::MSG_CHAT_BROAD,out.dump()};
        users_.broadcast(bp,conn);
    }

    void MessageHandler::handlePrivate(Connection* conn,const chat::protocol::Packet& pkt){
        if(Connection::AUTHENTICATED!=conn->state()){
            sendAck(conn,false,"未登录");
            return;
        }

        json j;
        try{j=json::parse(pkt.body);}
        catch(...){return;}

        std::string to=j.value(field::TO,"");
        std::string content=j.value(field::CONTENT,"");
        
        Connection* target=users_.get(to);
        if(!target){
            sendAck(conn,false,"对象用户不存在");
            return;
        }

        json out;
        out[field::FROM]=conn->username();
        out[field::CONTENT]=content;
        out[field::TO]=to;

        chat::protocol::Packet bp{chat::protocol::MSG_PRIVATE,out.dump()};
        //只交给目标用户
        target->send(bp);
    }

    void MessageHandler::handleUserList(Connection* conn){
        json j;
        j[field::USERS]=users_.usernames();
        chat::protocol::Packet bp{chat::protocol::MSG_USER_LIST,j.dump()};
        conn->send(bp);
    }

    void MessageHandler::handlePing(Connection* conn){
        chat::protocol::Packet bp{chat::protocol::MSG_PONG,""};
        conn->send(bp);
    }

    void MessageHandler::handleLogOut(Connection* conn){
        if(Connection::CLOSED==conn->state()){return;}

        std::string name=conn->username();
        users_.remove(conn);
        conn->close();
    }

    void MessageHandler::sendAck(Connection* conn,bool ok,const std::string& err){
        json j;
        j[field::OK]=ok;
        if(!ok){j[field::ERROR]=err;}
        conn->send({chat::protocol::MSG_LOGIN_ACK,j.dump()});
    }

    void MessageHandler::broadcastSystem(const std::string& text){
        json j;
        j[field::CONTENT]=text;
        j[field::TIME]=time(nullptr);
        users_.broadcast({chat::protocol::MSG_SYSTEM,j.dump()});
    }

    void MessageHandler::broadcastUserList(){
        json j;
        j[field::USERS]=users_.usernames();
        users_.broadcast({chat::protocol::MSG_USER_LIST,j.dump()});
    }

    void MessageHandler::onClose(Connection* conn){
        if(Connection::AUTHENTICATED!=conn->state()){return;}
        std::string name=conn->username();
        users_.remove(conn);
        broadcastSystem(name+"离线");
        broadcastUserList();
    }
}
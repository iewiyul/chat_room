#include "http_server.h"
#include "http_connection.h"
#include "ws_hub.h"
#include "ws_handshake.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace chat::server{
    HttpServer::HttpServer(Reactor& r, uint16_t port, std::string webroot,
                           const HttpDeps* deps)
        : reactor_(r), port_(port), webroot_(std::move(webroot)), deps_(deps){}
    HttpServer::~HttpServer(){stop();}

    bool HttpServer::start(){
        listenFd_=::socket(AF_INET,SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,0);
        if(listenFd_<0){perror("socket()");return false;}

        int yes=1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR,&yes, sizeof(yes));

        sockaddr_in addr;
        addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=htonl(INADDR_ANY);
        addr.sin_port=htons(port_);
        if(::bind(listenFd_,(sockaddr*)&addr,sizeof(addr))<0){perror("bind()");return false;}
        if(::listen(listenFd_,128)<0){perror("listen()");return false;}

        fprintf(stderr,"[http] start listenFd=%d port=%u webroot=%s\n",listenFd_,port_,webroot_.c_str());
        reactor_.addFd(listenFd_,EPOLLIN,[this](){onAccept_();},nullptr);

        return true;
    }

    void HttpServer::stop(){
        if(listenFd_>=0){
            reactor_.removeFd(listenFd_);
            ::close(listenFd_);
            listenFd_=-1;
        }
        conns_.clear();
    }

    void HttpServer::addRoute(const std::string& pathPrefix,RouteHandler h){
        routes_[pathPrefix]=std::move(h);
    }

    void HttpServer::onAccept_(){
        while(true){
            sockaddr_in cli{};
            socklen_t len=sizeof(cli);
            int cfd=::accept4(listenFd_,(sockaddr*)&cli,&len,SOCK_NONBLOCK | SOCK_CLOEXEC);
            if(cfd<0){
                if(errno==EAGAIN || errno==EWOULDBLOCK){return;}
                perror("accept()");
                return;
            }

            auto conn=std::make_unique<HttpConnection>(
                cfd,
                reactor_,
                [this](const HttpRequest& req,HttpConnection& c){
                    dispatch_(req,c);
                }
            );
            HttpConnection* raw=conn.get();

            // 关闭时把自己从 conns_ 摘掉，避免 unique_ptr 跟 fd 都不存在后
            // reactor 还持有死回调
            raw->setCloseCallback([this](HttpConnection* cc){
                if (cc->isUpgraded()){
                    // 关键顺序:必须先 capture 再 erase,否则 cc 会被析构
                    int fd = cc->fd();
                    std::string user = cc->upgradedUser();
                    if (deps_ && deps_->hub){
                        deps_->hub->adoptConnection(fd, user);
                    }
                    conns_.erase(fd);
                } else {
                    conns_.erase(cc->fd());
                }
            });
            raw->start();

            // 保活：HttpConnection 的所有权归 HttpServer
            conns_.emplace(cfd, std::move(conn));
        }
    }

    std::string HttpServer::mimeType(const std::string& path){
        auto dot = path.find_last_of('.');
        if (dot == std::string::npos) return "application/octet-stream";
        std::string ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                [](unsigned char c){ return std::tolower(c); });
        if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
        if (ext == "css")  return "text/css; charset=utf-8";
        if (ext == "js")   return "application/javascript; charset=utf-8";
        if (ext == "json") return "application/json; charset=utf-8";
        if (ext == "png")  return "image/png";
        if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
        if (ext == "gif")  return "image/gif";
        if (ext == "svg")  return "image/svg+xml";
        if (ext == "ico")  return "image/x-icon";
        if (ext == "txt")  return "text/plain; charset=utf-8";
        return "application/octet-stream";
    }

    void HttpServer::dispatch_(const HttpRequest& req,HttpConnection& conn){
        // 阶段 3：WS upgrade 检测（在路由表之前,优先级最高）
        // 要求:GET /ws + Upgrade: websocket + Connection: Upgrade
        auto upg = req.getHeader("Upgrade");
        auto con = req.getHeader("Connection");
        bool isUpgrade = (req.method() == "GET")
            && req.path() == "/ws"
            && !upg.empty()
            && (upg == "websocket" || upg == "WebSocket")
            && (con.find("Upgrade") != std::string::npos);
        if (isUpgrade){
            handleWsUpgrade_(req, conn);
            return;
        }

        for(auto& [prefix,h]:routes_){
            if(req.path().rfind(prefix,0)==0){
                h(req,conn);
                return;
            }
        }

        //静态路由
        std::string url=req.path();
        if (url == "/") url = "/index.html";
        if(url.find("..")!=std::string::npos){
            HttpResponse r=HttpResponse::makeError(403,"Forbidden");
            conn.send(r);
            return;
        }

        std::string full=webroot_+url;
        std::string served=url;     // 实际服务的路径(可能加了 .html),给 mimeType 用
        // 阶段 4:无扩展名时,先试 .html(如 /chat → /chat.html)
        if (url.find_last_of('.') == std::string::npos){
            std::ifstream probe(full + ".html", std::ios::binary);
            if (probe){
                full += ".html";
                served += ".html";
            }
        }
        std::ifstream f(full,std::ios::binary);
        if(!f){
            HttpResponse r=HttpResponse::makeError(404,"Not Found");
            conn.send(r);
            return;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string body=ss.str();

        HttpResponse r(200);
        r.contentType(mimeType(served));
        r.body(body);
        conn.send(r);
    }

    // =========================================================================
    // handleWsUpgrade_ —— 阶段 3：处理 WebSocket 升级握手
    //
    // 流程：
    //   1. 验 token（?token=xxx → TokenStore::lookup）
    //   2. 验 Sec-WebSocket-Version == 13
    //   3. 算 Sec-WebSocket-Accept
    //   4. 构造 101 响应（closeAfterWrite_=true 自动触发 close）
    //   5. conn.markUpgraded(username) → close() 会跳 ::close,fd 转交 WsHub
    //   6. conn.send(101) → onWrite flush → close() → onClose_ 回调
    //      → HttpServer::onAccept_ 里 setCloseCallback 检测 isUpgraded
    //      → WsHub::adoptConnection(fd, username)
    // =========================================================================
    void HttpServer::handleWsUpgrade_(const HttpRequest& req, HttpConnection& conn){
        if (!deps_ || !deps_->tokens || !deps_->hub){
            conn.send(HttpResponse::makeError(500, "server not configured for WS"));
            return;
        }

        // 1. 验 token
        std::string token = req.getQuery("token");
        std::string user  = (token.empty()) ? std::string() : deps_->tokens->lookup(token);
        if (user.empty()){
            conn.send(HttpResponse::makeError(401, "invalid token"));
            return;
        }

        // 2. 验版本
        std::string ver = req.getHeader("Sec-WebSocket-Version");
        if (ver != "13"){
            conn.send(HttpResponse::makeError(400, "Sec-WebSocket-Version must be 13"));
            return;
        }

        // 3. 算 Sec-WebSocket-Accept
        std::string key = req.getHeader("Sec-WebSocket-Key");
        if (key.empty()){
            conn.send(HttpResponse::makeError(400, "missing Sec-WebSocket-Key"));
            return;
        }
        std::string accept = ws::computeAcceptKey(key);

        // 4. 标记 upgraded（关键:close() 不会 ::close fd）
        conn.markUpgraded(user);

        // 5. 构造 101 响应（keepAlive=false → closeAfterWrite_=true → onWrite flush 后 close）
        HttpResponse r;
        r.status(101, "Switching Protocols");
        r.header("Upgrade", "websocket");
        r.header("Connection", "Upgrade");
        r.header("Sec-WebSocket-Accept", accept);
        conn.send(r);

        // 此后：
        //   onWrite 把 101 字节发出去 → writeBuf 空 + closeAfterWrite_=true
        //   → close() 触发 → onClose_(this) 回调
        //   → HttpServer 在 onAccept_ 设置的 close callback 检测 cc->isUpgraded()
        //   → 调 deps_->hub->adoptConnection(cc->fd(), cc->upgradedUser())
        //   → conns_.erase + unique_ptr 析构 HttpConnection
    }
}
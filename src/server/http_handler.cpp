#include "http_handler.h"
#include "http_server.h"

#include <chrono>

namespace chat::server{

HttpResponse jsonResp(int code, const nlohmann::json& j){
    HttpResponse r(code);
    r.bodyJson(j.dump());
    return r;
}

static int64_t nowMs(){
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

static std::string requireToken(const HttpRequest& req, const TokenStore& ts,
                                HttpConnection& conn, int& outStatus){
    // token 在 ?token=xxx 里，或 body JSON 里
    std::string tok = req.getQuery("token");
    if (tok.empty() && !req.body().empty()){
        try{
            auto j = nlohmann::json::parse(req.body());
            if (j.contains("token")) tok = j["token"].get<std::string>();
        } catch (...) {}
    }
    if (tok.empty()){
        conn.send(jsonResp(401, {{"error","missing token"}}));
        outStatus = 401;
        return "";
    }
    std::string u = ts.lookup(tok);
    if (u.empty()){
        conn.send(jsonResp(401, {{"error","invalid token"}}));
        outStatus = 401;
        return "";
    }
    return u;
}

void registerHttpRoutes(HttpServer& s, const HttpDeps& deps){

    // POST /api/login
    s.addRoute("/api/login", [deps](const HttpRequest& req, HttpConnection& c){
        if (req.method() != "POST"){
            c.send(jsonResp(405, {{"error","method not allowed"}}));
            return;
        }
        nlohmann::json j;
        try { j = nlohmann::json::parse(req.body()); }
        catch (...) { c.send(jsonResp(400, {{"error","bad json"}})); return; }

        std::string u = j.value("username", "");
        if (u.empty()){
            c.send(jsonResp(400, {{"error","missing username"}}));
            return;
        }
        if (deps.room->hasUser(u)){
            c.send(jsonResp(409, {{"error","username taken"}}));
            return;
        }
        deps.room->addUser(u);
        std::string tok = deps.tokens->issue(u);
        c.send(jsonResp(200, {{"ok",true},{"token",tok},{"username",u}}));
    });

    // GET /api/users
    s.addRoute("/api/users", [deps](const HttpRequest& req, HttpConnection& c){
        int st = 0;
        std::string u = requireToken(req, *deps.tokens, c, st);
        if (st != 0) return;
        auto users = deps.room->users();
        c.send(jsonResp(200, {{"users", users}}));
    });

    // /api/messages —— GET 拿历史, POST 发一条（合并到同一个 path prefix）
    s.addRoute("/api/messages", [deps](const HttpRequest& req, HttpConnection& c){
        int st = 0;
        std::string u = requireToken(req, *deps.tokens, c, st);
        if (st != 0) return;

        if (req.method() == "GET"){
            int64_t since = 0;
            std::string s_since = req.getQuery("since");
            if (!s_since.empty()){
                try { since = std::stoll(s_since); } catch (...) {}
            }
            auto msgs = deps.room->recentMessages(since);
            nlohmann::json arr = nlohmann::json::array();
            for (auto& m : msgs) arr.push_back(m.toJson());
            c.send(jsonResp(200, {{"messages", arr}}));
            return;
        }

        if (req.method() == "POST"){
            nlohmann::json j;
            try { j = nlohmann::json::parse(req.body()); }
            catch (...) { c.send(jsonResp(400, {{"error","bad json"}})); return; }

            std::string content = j.value("content", "");
            if (content.empty()){
                c.send(jsonResp(400, {{"error","empty content"}}));
                return;
            }
            Message m{u, "lobby", content, nowMs()};
            deps.room->addMessage(m);
            c.send(jsonResp(200, {{"ok",true}}));
            return;
        }

        c.send(jsonResp(405, {{"error","method not allowed"}}));
    });

    // POST /api/logout
    s.addRoute("/api/logout", [deps](const HttpRequest& req, HttpConnection& c){
        int st = 0;
        std::string u = requireToken(req, *deps.tokens, c, st);
        if (st != 0) return;

        // 拿 token 来 revoke
        std::string tok = req.getQuery("token");
        if (tok.empty()){
            try {
                auto j = nlohmann::json::parse(req.body());
                if (j.contains("token")) tok = j["token"].get<std::string>();
            } catch (...) {}
        }
        if (!tok.empty()){
            deps.tokens->revoke(tok);
        }
        deps.room->removeUser(u);
        c.send(jsonResp(200, {{"ok",true}}));
    });
}
}
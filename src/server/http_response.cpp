#include "http_response.h"
#include <algorithm>
#include <sstream>
  
namespace chat::server{
    static std::string defaultReason(int code){
        switch(code){
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 411: return "Length Required";
            case 413: return "Payload Too Large";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            default:  return "Unknown";
        }
    }

    HttpResponse::HttpResponse()=default;
    HttpResponse::HttpResponse(int code):statusCode_(code),reason_(defaultReason(code)){}

    HttpResponse& HttpResponse::status(int code,const std::string& reason){
        statusCode_=code;
        reason_=reason.empty()?defaultReason(code):reason;
        return *this;
    }

    HttpResponse& HttpResponse::header(const std::string& k,const std::string& v){
        headers_[k]=v;
        return *this;
    }

    HttpResponse& HttpResponse::contentType(const std::string& mime){
        headers_["Content-Type"]=mime;
        return *this;
    }

    HttpResponse& HttpResponse::body(const std::string& s){
        body_=s;
        return *this;
    }

    HttpResponse& HttpResponse::bodyJson(const std::string& jsonStr){
        headers_["Content-Type"]="application/json";
        body_=jsonStr;
        return *this;
    }

    std::string HttpResponse::toString() const{
        std::ostringstream out;
        out << "HTTP/1.1 " << statusCode_ << " " << reason_ << "\r\n";

        bool hasCL=false, hasConn=false;
        for(auto& kv:headers_){
            std::string lk;
            lk.resize(kv.first.size());
            std::transform(kv.first.begin(),kv.first.end(),lk.begin(),
            [](unsigned char c){return std::tolower(c);});
            if(lk=="content-length"){hasCL=true;}
            if(lk=="connection"){hasConn=true;}
            out << kv.first << ':' << kv.second << "\r\n";
        }
        if(!hasCL){
            out << "Content-Length: " << body_.size() << "\r\n";
        }
        if(!hasConn){
            out << "Connection: " << (keepAlive_?"keep-alive":"close") << "\r\n";
        }
        out << "\r\n";
        out << body_;
        return out.str();
    }

    HttpResponse HttpResponse::makeError(int code,const std::string& msg){
        HttpResponse r(code);
        std::string body="{\"error\":\"" + msg + "\"}";
        r.headers_["Content-Type"]="application/json";
        r.body_=body;
        return r;
    }

    HttpResponse HttpResponse::makeJson(int code,const std::string& jsonStr){
        HttpResponse r(code);
        r.body_ = jsonStr;
        r.headers_["Content-Type"] = "application/json";
        return r;
    }
}

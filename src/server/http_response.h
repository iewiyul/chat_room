#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace chat::server{
    class HttpResponse{
    public:
        HttpResponse();
        //给http赋予错误码
        explicit HttpResponse(int statusCode);

        //设置http请求的状态
        HttpResponse& status(int code,const std::string& reason="");
        //设置headers的键值对
        HttpResponse& header(const std::string& k,const std::string& v);
        //设置headers的contenttype
        HttpResponse& contentType(const std::string& mime);
        //设置不同body类型的解析方式
        HttpResponse& body(const std::string& s);
        HttpResponse& bodyJson(const std::string& jsonStr);
        
        //把http请求序列化为字符串
        std::string toString() const;

        int statusCode() const{return statusCode_;}
        bool keepAlive() const{return keepAlive_;}
        void setKeepAlive(bool ka){keepAlive_ = ka;}

        static HttpResponse makeError(int code,const std::string& msg);
        static HttpResponse makeJson(int code,const std::string& jsonStr);

    private:
        int statusCode_=200;
        std::string reason_="OK";
        std::unordered_map<std::string,std::string> headers_;
        std:: string body_;
        bool keepAlive_=false;
    };
}
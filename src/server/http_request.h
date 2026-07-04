#pragma once
#include <string>
#include <unordered_map>
#include "../common/buffer.h"

namespace chat::server{
    class HttpRequest{
    public:
        //解析整个http请求
        bool parse(chat::common::Buffer& buf);

        bool ok() const{return ok_;}

        const std::string& method() const{return method_;}
        const std::string& path() const{return path_;}
        const std::string& query() const{return query_;}
        const std::string& version() const{return version_;}
        const std::string& body() const{return body_;}
        //获得http请求的请求头
        const std::string& getHeader(const std::string& k) const;

        //获得字段的值
        std::string getQuery(const std::string& k) const;
    private:
        bool ok_=false;
        std::string method_;
        std::string path_;
        std::string query_;
        std::string version_;
        std::unordered_map<std::string,std::string> headers_;
        std::string body_;

        //解析http请求的第一行
        bool parseRequestLine(const std::string line);
        //解析http请求的第二个部分
        void parseHeaders(const std::string& block);
    };
}
#include "http_request.h"
#include <algorithm>

namespace chat::server{
    //把字符串转换成小写
    static std::string toLower(std::string s){
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c){ return std::tolower(c); });
        return s;
    } 
    
    //去掉字符串两端的空格
    static std::string trim(const std::string& s){
        auto a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        auto b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    } 

    const std::string& HttpRequest::getHeader(const std::string& k) const{
        static const std::string EMPTY;
        auto it=headers_.find(toLower(k));
        return it==headers_.end()?EMPTY:it->second;
    }

    std::string HttpRequest::getQuery(const std::string& k) const{
        if(query_.empty()){return "";}
        size_t pos=0;
        while(pos<query_.size()){
            size_t eq=query_.find('=',pos);
            if(eq==std::string::npos){break;}
            size_t amp=query_.find('&',eq);
            std::string key=query_.substr(pos,eq-pos);
            std::string val=query_.substr(eq+1,
            amp==std::string::npos?std::string::npos:amp-eq-1);
            if(key==k){return val;}
            if(amp==std::string::npos){break;}
            pos=amp+1;
        }
        return "";
    }

    bool HttpRequest::parseRequestLine(const std::string line){
        size_t sp1=line.find(' ');
        if(sp1==std::string::npos){return false;}
        size_t sp2=line.find(' ',sp1+1);
        if(sp2==std::string::npos){return false;}
        method_=line.substr(0,sp1);
        std::string url=line.substr(sp1+1,sp2-sp1-1);
        version_=line.substr(sp2+1);

        if(version_!="HTTP/1.1" && version_!="HTTP/1.0"){return false;}

        size_t q=url.find('?');
        if(q==std::string::npos){
            path_=url;
            query_.clear();
        }else{
            path_=url.substr(0,q);
            query_=url.substr(q+1);
        }
        if(path_.empty()){return false;}
        return true;
    }

    void HttpRequest::parseHeaders(const std::string& block){
        size_t start=0;
        while(start<block.size()){
            size_t end=block.find("\r\n",start);
            if(end==std::string::npos){end=block.size();}
            std::string line=block.substr(start,end-start);
            size_t colon=line.find(':');
            if(colon!=std::string::npos){
                std::string k=toLower(trim(line.substr(0,colon)));
                std::string v=trim(line.substr(colon+1));
                headers_[k]=v;
            }
            if(end==block.size()){break;}
            start=end+2;
        }
    }

    bool HttpRequest::parse(chat::common::Buffer& buf){
        ok_=false;

        std::string snap=buf.retrieveAllAsString();
        size_t hdr_end=snap.find("\r\n\r\n");
        if(hdr_end==std::string::npos){buf.append(snap);return false;}

        std::string head=snap.substr(0,hdr_end);
        std::string rest=snap.substr(hdr_end+4);

        size_t line_end=head.find("\r\n");
        std::string req_line=(line_end==std::string::npos)?head:head.substr(0,line_end);
        if(!parseRequestLine(req_line)){
            buf.append(snap);
            return false;
        }
        if(line_end!=std::string::npos){
            parseHeaders(head.substr(line_end+2));
        }

        size_t content_length=0;
        try{
            content_length=std::stoul(getHeader("Content-Length"));
        }catch(...){content_length=0;}

        if(content_length>rest.size()){
            buf.append(snap);
            return false;
        }

        body_=rest.substr(0,content_length);
        if(content_length<rest.size()){
            buf.append(rest.substr(content_length));
        }

        ok_=true;
        return true;
    }
}

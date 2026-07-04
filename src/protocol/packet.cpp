#include "packet.h"
#include "message.h"
#include <arpa/inet.h>
#include <cstring>
#include <string>

namespace chat::protocol{
    //编码函数
    std::vector<uint8_t> encode(const Packet& pkt){
        uint32_t bodyLength=pkt.body.size()+TYPE_SIZE;

        std::vector<uint8_t> out(HEADER_SIZE+bodyLength);
        
        //1.放入4字节
        uint32_t netLen=htonl(bodyLength);
        memcpy(out.data(),&netLen,HEADER_SIZE);

        //2.放入类型
        out[HEADER_SIZE]=pkt.type;

        //3.放入主体内容
        if(!pkt.body.empty()){
            memcpy(out.data()+HEADER_SIZE+TYPE_SIZE,pkt.body.data(),pkt.body.size());
        }

        return out;
    }

    //解码函数
    DecodeResult decode(const uint8_t* data,size_t len){
        DecodeResult result{0,{},false};

        //数据还不够长度字段
        if(len<HEADER_SIZE){
            result.consumed=-1;
            return result;
        }

        //读出长度
        uint32_t netLen,bodyLen;
        memcpy(&netLen,data,HEADER_SIZE);
        bodyLen=ntohl(netLen);

        //判断长度是否合法‘
        if(bodyLen<TYPE_SIZE || bodyLen>MAX_BODY_LENGTH){
            return result;
        }

        //判断数据是否完整
        size_t totalLen=HEADER_SIZE+bodyLen;
        if(len<totalLen){
            result.consumed=-1;
            return result;
        }

        //解包
        result.packet.type=data[HEADER_SIZE];
        result.packet.body.assign(reinterpret_cast<const char*>(data+HEADER_SIZE+TYPE_SIZE),bodyLen-TYPE_SIZE);
        result.consumed=static_cast<int>(totalLen);
        result.ok=true;

        return result;
    }
}
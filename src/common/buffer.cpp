#include "buffer.h"
#include <cstring>

namespace chat::common{
    void Buffer::append(const uint8_t* data,size_t len){
        if(0==len){return;}
        makeSpace(len);
        memcpy(buf_.data()+writePos_,data,len);
        writePos_+=len;
    }

    void Buffer::append(const std::string& s){
        append(reinterpret_cast<const uint8_t*>(s.data()),s.size());
    }

    const uint8_t* Buffer::peek() const{
        return buf_.data()+readPos_;
    }

    size_t Buffer::readableBytes() const{
        return writePos_-readPos_;
    }

    void Buffer::consume(size_t len){
        if(len>readableBytes()){len=readableBytes();}
        readPos_+=len;
    }

    std::string Buffer::retrieveAllAsString(){
        std::string s(reinterpret_cast<const char*>(peek()),readableBytes());
        clear();
        return s;
    }

    uint8_t* Buffer::writeBegin(){
        return buf_.data()+writePos_;
    }

    size_t Buffer::writableBytes() const{
        return buf_.size()-writePos_;
    }

    void Buffer::hasWritten(size_t len){
        writePos_+=len;
    }

    void Buffer::clear(){
        readPos_=0;
        writePos_=0;
    }

    void Buffer::makeSpace(size_t len){
        //剩余有空间
        if(writableBytes()>=len){return;}

        //剩余空间不够，但是可以压缩
        if(readPos_+writableBytes()>=len){
            size_t readable=readableBytes();
            std::memmove(buf_.data(),buf_.data()+readPos_,readable);
            readPos_=0;
            writePos_=readable;
            return;
        }

        //压缩也不够
        size_t newCap=buf_.size()*2;
        while((newCap-writePos_)<len){newCap*=2;}
        buf_.resize(newCap);
    }
}
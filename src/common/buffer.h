#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace chat::common{
    class Buffer{
    public:
        static const size_t INITIAL_SIZE=1024;

        Buffer():buf_(INITIAL_SIZE),readPos_(0),writePos_(0){};

        //追加数据
        void append(const uint8_t* data,size_t len);
        void append(const std::string& s);

        //查看
        const uint8_t* peek() const;
        size_t readableBytes() const;

        //消费
        void consume(size_t len);
        std::string retrieveAllAsString();

        //直接写入
        uint8_t* writeBegin();
        size_t writableBytes() const;
        void hasWritten(size_t len);

        void clear();

    private:
        std::vector<uint8_t> buf_;
        size_t readPos_;
        size_t writePos_;

        void makeSpace(size_t len);
    };
}
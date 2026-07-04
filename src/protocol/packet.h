#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace chat::protocol{
    struct Packet{
        uint8_t type;
        std::string body;
    };

    //编码函数
    std::vector<uint8_t> encode(const Packet& pkt);

    struct DecodeResult{
        int consumed;
        Packet packet;
        bool ok;
    };
    //解码函数
    DecodeResult decode(const uint8_t* data,size_t len);
}
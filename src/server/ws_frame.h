#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace chat::server::ws{

// RFC 6455 §5.2  Base Framing Protocol — opcodes
enum Opcode : uint8_t{
    CONTINUATION = 0x0,
    TEXT         = 0x1,
    BINARY       = 0x2,
    CLOSE        = 0x8,
    PING         = 0x9,
    PONG         = 0xA,
};

struct Frame{
    bool fin = false;
    uint8_t opcode = 0;            // 参见 Opcode
    std::vector<uint8_t> payload;  // 已经解开 mask 的明文
};

// tryDecode 的返回值：
//   consumed > 0  → 成功解码一帧（payload 已去 mask）
//   consumed == -1 → 数据不够，需要更多 recv 后再试
//   consumed == 0  → 协议错误（连接应关闭）
struct DecodeResult{
    int consumed;
    Frame frame;
};

// 输入 data/len 是 readBuf_ 的可见部分（peek 起点 + readableBytes）。
// 一次性尽量解一帧；连续调用由 WsConnection::onRead 在 while 循环里驱动。
DecodeResult tryDecode(const uint8_t* data, size_t len);

// 服务端→客户端帧（FIN=1, MASK=0）。不处理 fragmentation。
std::vector<uint8_t> encodeFrame(uint8_t opcode, const void* payload, size_t len);

inline std::vector<uint8_t> encodeText(const std::string& s){
    return encodeFrame(TEXT, s.data(), s.size());
}
inline std::vector<uint8_t> encodePing(const std::string& s){
    return encodeFrame(PING, s.data(), s.size());
}
inline std::vector<uint8_t> encodePong(const std::string& s){
    return encodeFrame(PONG, s.data(), s.size());
}
inline std::vector<uint8_t> encodeClose(uint16_t code){
    uint8_t p[2] = { uint8_t(code >> 8), uint8_t(code & 0xFF) };
    return encodeFrame(CLOSE, p, 2);
}
inline std::vector<uint8_t> encodeClose(){
    return encodeFrame(CLOSE, nullptr, 0);
}

}  // namespace chat::server::ws
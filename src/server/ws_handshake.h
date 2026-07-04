#pragma once

#include <string>

namespace chat::server::ws{

// RFC 6455 §1.3 —— 服务端握手的 Sec-WebSocket-Accept 计算
//
//   accept = base64( sha1( Sec-WebSocket-Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )
//
// 返回的 accept 字符串直接写到 101 响应的 Sec-WebSocket-Accept 头里。
//
// clientKey 必须是浏览器 / 客户端发送的原始 Sec-WebSocket-Key 值(已经是 base64 字符串)。
std::string computeAcceptKey(const std::string& clientKey);

// 便捷：生成"魔法 GUID"常量（RFC 6455 定义,不修改）
inline constexpr const char* MAGIC_GUID =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

}  // namespace chat::server::ws
#include "ws_frame.h"
#include <cstring>
#include <vector>

namespace chat::server::ws{

// =========================================================================
// tryDecode —— 把一段字节流解成一帧
//
// 线缆格式 (RFC 6455 §5.2):
//   byte 0:        FIN(1)  RSV1-3(3)  opcode(4)
//   byte 1:        MASK(1) payload_len(7)
//   if len==126:   Extended payload length (2 bytes, big-endian)
//   if len==127:   Extended payload length (8 bytes, big-endian)
//   if MASK==1:    Masking-key (4 bytes)
//   payload:       payload_len bytes (XOR masking-key 循环)
//
// 客户端 → 服务端: MASK 必须 = 1,不解开就当作协议错误（consumed=0）
// =========================================================================
DecodeResult tryDecode(const uint8_t* data, size_t len){
    // TODO: 实现 tryDecode
    //
    // 步骤：
    //   1. len < 2 → 还没收到完整首部,返回 {-1, {}}
        if(len<2){return {-1,{}};}
    //   2. 解 byte 0：
    //        fin    = (b0 >> 7) & 1
    //        opcode = b0 & 0x0F
    //      RSV1-3 不为 0 → return {0, {}}  （没协商扩展协议）
        uint8_t b0=*data;
        bool fin=(b0>>7)&1;
        uint8_t opcode=b0 & (0x0F);
        uint8_t rsv=b0 & (0x70);
        if((rsv | (0x8F))!=0x8F){return {0,{}};}
    //   3. 解 byte 1：
    //        mask = (b1 >> 7) & 1
    //        plen = b1 & 0x7F
    //      pos = 2
        uint8_t b1=*(data+1);
        uint8_t mask=(b1>>7) & 1;
        uint64_t plen=b1 & 0x7F;
        int pos=2;
    //   4. 处理扩展长度：
    //        plen == 126 → 读 2 字节大端, pos += 2
    //        plen == 127 → 读 8 字节大端, pos += 8
    //      数据不够就 return {-1, {}}
        if(plen == 126){
            if(len < pos + 2) return {-1,{}};                          // 数据不够 → 等
            plen = (uint64_t(data[pos]) << 8) | uint64_t(data[pos+1]); // 大端 2 字节
            pos += 2;
        } else if(plen == 127){
            if(len < pos + 8) return {-1,{}};                          // 数据不够 → 等
            plen = 0; 
            for(int i=0; i<8; ++i){
                plen = (plen << 8) | uint64_t(data[pos+i]);            // 大端 8 字节
            }
            pos += 8;
        }
    //   5. 处理 masking-key：
    //        mask == 1 → 读 4 字节到 mkey[], pos += 4
    //        mask == 0 → return {0, {}}  （客户端必须 mask）
    //      数据不够就 return {-1, {}}
        std::vector<uint8_t> mkey(4);
        if(1==mask){
            for(int i=0;i<4;i++){
                mkey[i]=*(data+i+pos);
            }
            pos+=4;
        }else if(0==mask){
            return {0,{}};
        }

        if(len<pos+plen){return {-1,{}};}
    //   7. XOR 解 mask 填进 Frame.payload（每个字节 ^ mkey[i % 4]）
        Frame f;
        f.fin = fin;
        f.opcode = opcode;
        f.payload.resize(plen);                // 先开够空间
        for(uint64_t i = 0; i < plen; ++i){
            f.payload[i] = data[pos + i] ^ mkey[i % 4];   // i%4 在 [0..3] 循环
        }
    //   8. return { int(pos + plen), Frame{fin, opcode, payload} }
        return {int(pos+plen),f};
}       

// =========================================================================
// encodeFrame —— 服务端→客户端帧（FIN=1, MASK=0, 单帧）
//
// 长度编码策略：
//   len < 126      → 第 2 字节 = len
//   len < 65536    → 第 2 字节 = 126, 后跟 2 字节大端长度
//   len >= 65536   → 第 2 字节 = 127, 后跟 8 字节大端长度
// =========================================================================
std::vector<uint8_t> encodeFrame(uint8_t opcode, const void* payload, size_t len){
    std::vector<uint8_t> out;
    out.reserve(2 + 8 + len);   // 2 首部 + 最多 8 扩展长度 + payload

    // byte 0: FIN=1, RSV=0, opcode
    out.push_back(0x80 | (opcode & 0x0F));

    // byte 1 起: MASK=0（服务端从不对客户端 mask）+ payload_len
    if (len < 126){
        out.push_back(uint8_t(len));
    } else if (len < 65536){
        out.push_back(126);
        out.push_back(uint8_t(len >> 8));
        out.push_back(uint8_t(len & 0xFF));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i){
            out.push_back(uint8_t((len >> (i * 8)) & 0xFF));
        }
    }

    // payload（payload 可能为 null,例如 encodeClose() 无 body,insert 不做任何事）
    const uint8_t* p = static_cast<const uint8_t*>(payload);
    out.insert(out.end(), p, p + len);

    return out;
}

}  // namespace chat::server::ws
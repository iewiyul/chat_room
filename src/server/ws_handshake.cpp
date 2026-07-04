#include "ws_handshake.h"

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

namespace chat::server::ws{

// =========================================================================
// SHA1 —— RFC 3174。20 字节摘要。
// 输入任意长字节流,输出 20 字节。
// =========================================================================
namespace{

struct Sha1{
    uint32_t h[5];        // 5 个 32-bit state
    uint64_t totalBits;   // 累计处理的 bit 数
    uint8_t  buf[64];     // 未满 64 字节的中间 buffer
    size_t   bufLen;      // buf 已用字节数

    void init();
    void update(const uint8_t* data, size_t len);
    void final(uint8_t out[20]);

private:
    void processBlock(const uint8_t block[64]);
};

void Sha1::init(){
    h[0] = 0x67452301;
    h[1] = 0xEFCDAB89;
    h[2] = 0x98BADCFE;
    h[3] = 0x10325476;
    h[4] = 0xC3D2E1F0;
    totalBits = 0;
    bufLen = 0;
}

static inline uint32_t rotl(uint32_t x, int n){
    return (x << n) | (x >> (32 - n));
}

void Sha1::processBlock(const uint8_t block[64]){
    uint32_t W[80];

    // 1. 64 字节 → 16 个 uint32_t W[0..15]（大端）
    for (int i = 0; i < 16; ++i){
        W[i] = (uint32_t(block[4*i]) << 24)
             | (uint32_t(block[4*i+1]) << 16)
             | (uint32_t(block[4*i+2]) << 8)
             |  uint32_t(block[4*i+3]);
    }

    // 2. 扩展 W[16..79]
    for (int i = 16; i < 80; ++i){
        W[i] = rotl(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1);
    }

    // 3. 初始化 a..e
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    // 4. 主循环 80 轮
    for (int t = 0; t < 80; ++t){
        uint32_t f, K;
        if (t < 20){
            f = (b & c) | ((~b) & d);
            K = 0x5A827999;
        } else if (t < 40){
            f = b ^ c ^ d;
            K = 0x6ED9EBA1;
        } else if (t < 60){
            f = (b & c) | (b & d) | (c & d);
            K = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            K = 0xCA62C1D6;
        }
        uint32_t temp = rotl(a, 5) + f + e + K + W[t];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }

    // 5. 加回 state
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void Sha1::update(const uint8_t* data, size_t len){
    totalBits += uint64_t(len) * 8;

    // 先填满现有 buf
    if (bufLen > 0){
        size_t need = 64 - bufLen;
        if (len < need){
            std::memcpy(buf + bufLen, data, len);
            bufLen += len;
            return;
        }
        std::memcpy(buf + bufLen, data, need);
        processBlock(buf);
        data += need;
        len  -= need;
        bufLen = 0;
    }

    // 处理完整 64 字节块
    while (len >= 64){
        processBlock(data);
        data += 64;
        len  -= 64;
    }

    // 存剩余
    if (len > 0){
        std::memcpy(buf, data, len);
        bufLen = len;
    }
}

void Sha1::final(uint8_t out[20]){
    // 1. 追加 0x80
    buf[bufLen++] = 0x80;

    // 2. 如果 bufLen > 56,要先消化再开新块
    if (bufLen > 56){
        while (bufLen < 64) buf[bufLen++] = 0;
        processBlock(buf);
        bufLen = 0;
    }

    // 3. 补 0 到 56 字节
    while (bufLen < 56) buf[bufLen++] = 0;

    // 4. 写 totalBits (大端 8 字节)
    for (int i = 0; i < 8; ++i){
        buf[56 + i] = uint8_t((totalBits >> (56 - i*8)) & 0xFF);
    }

    // 5. 处理最后一块
    processBlock(buf);

    // 6. 大端输出 5 个 state
    for (int i = 0; i < 5; ++i){
        out[4*i]   = uint8_t((h[i] >> 24) & 0xFF);
        out[4*i+1] = uint8_t((h[i] >> 16) & 0xFF);
        out[4*i+2] = uint8_t((h[i] >>  8) & 0xFF);
        out[4*i+3] = uint8_t( h[i]        & 0xFF);
    }
}

}  // namespace


// =========================================================================
// base64 —— RFC 4648。把 20 字节 SHA1 输出编码成 base64 字符串。
// =========================================================================
static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len){
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= len){
        uint8_t b0 = data[i], b1 = data[i+1], b2 = data[i+2];
        out.push_back(B64_ALPHABET[ b0 >> 2 ]);
        out.push_back(B64_ALPHABET[ ((b0 & 0x03) << 4) | (b1 >> 4) ]);
        out.push_back(B64_ALPHABET[ ((b1 & 0x0F) << 2) | (b2 >> 6) ]);
        out.push_back(B64_ALPHABET[ b2 & 0x3F ]);
        i += 3;
    }

    // 剩余 1 或 2 字节
    size_t rem = len - i;
    if (rem == 1){
        uint8_t b0 = data[i];
        out.push_back(B64_ALPHABET[ b0 >> 2 ]);
        out.push_back(B64_ALPHABET[ (b0 & 0x03) << 4 ]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2){
        uint8_t b0 = data[i], b1 = data[i+1];
        out.push_back(B64_ALPHABET[ b0 >> 2 ]);
        out.push_back(B64_ALPHABET[ ((b0 & 0x03) << 4) | (b1 >> 4) ]);
        out.push_back(B64_ALPHABET[ (b1 & 0x0F) << 2 ]);
        out.push_back('=');
    }

    return out;
}


// =========================================================================
// computeAcceptKey —— 公开接口
// =========================================================================
std::string computeAcceptKey(const std::string& clientKey){
    std::string combined = clientKey + MAGIC_GUID;

    Sha1 s;
    s.init();
    s.update(reinterpret_cast<const uint8_t*>(combined.data()), combined.size());
    uint8_t digest[20];
    s.final(digest);

    return base64Encode(digest, 20);
}

}  // namespace chat::server::ws
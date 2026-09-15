// base64.h — 标准字母表编码（frame 事件的 JPEG 载荷用）
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace vl {

inline std::string b64_encode(const uint8_t* data, size_t len) {
    static const char kTab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kTab[(v >> 18) & 63];
        out += kTab[(v >> 12) & 63];
        out += kTab[(v >> 6) & 63];
        out += kTab[v & 63];
    }
    if (i + 1 == len) {  // 剩 1 字节
        uint32_t v = data[i] << 16;
        out += kTab[(v >> 18) & 63];
        out += kTab[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == len) {  // 剩 2 字节
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += kTab[(v >> 18) & 63];
        out += kTab[(v >> 12) & 63];
        out += kTab[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

inline std::string b64_encode(const std::vector<uint8_t>& v) {
    return b64_encode(v.data(), v.size());
}

}  // namespace vl

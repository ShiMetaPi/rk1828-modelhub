// json.h — 本协议专用的极简 JSON 工具（自产自销，不追求通用）
// 协议只有两种消息：{"cmd":"ask"/"reset",...} 和 {"ev":"frame/token/done/error",...}
#pragma once
#include <string>
#include <cstdint>

namespace vl {

// 转义成 JSON 字符串字面量（含两侧引号）。UTF-8 字节原样透传。
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {  // 其余控制字符
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
    return out;
}

// 从一行 JSON 里取整型字段值（"qid":123）。找不到返回 def。
inline int64_t json_int_field(const std::string& line, const char* key, int64_t def) {
    std::string pat = "\"" + std::string(key) + "\"";
    size_t p = line.find(pat);
    if (p == std::string::npos) return def;
    p = line.find(':', p + pat.size());
    if (p == std::string::npos) return def;
    ++p;
    while (p < line.size() && line[p] == ' ') ++p;
    bool neg = false;
    if (p < line.size() && line[p] == '-') { neg = true; ++p; }
    int64_t v = 0; bool any = false;
    while (p < line.size() && line[p] >= '0' && line[p] <= '9') {
        v = v * 10 + (line[p] - '0'); ++p; any = true;
    }
    if (!any) return def;
    return neg ? -v : v;
}

// 从一行 JSON 里取字符串字段值（"text":"..."），处理 \" \\ \n \t \r \uXXXX 转义。
inline bool json_str_field(const std::string& line, const char* key, std::string* out) {
    std::string pat = "\"" + std::string(key) + "\"";
    size_t p = line.find(pat);
    if (p == std::string::npos) return false;
    p = line.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < line.size() && line[p] == ' ') ++p;
    if (p >= line.size() || line[p] != '"') return false;
    ++p;
    out->clear();
    while (p < line.size()) {
        char c = line[p];
        if (c == '"') return true;  // 正常收尾
        if (c == '\\') {
            ++p;
            if (p >= line.size()) break;
            char e = line[p];
            switch (e) {
                case '"':  *out += '"';  break;
                case '\\': *out += '\\'; break;
                case '/':  *out += '/';  break;
                case 'n':  *out += '\n'; break;
                case 't':  *out += '\t'; break;
                case 'r':  *out += '\r'; break;
                case 'b':  *out += '\b'; break;
                case 'f':  *out += '\f'; break;
                case 'u': {  // \uXXXX → UTF-8
                    if (p + 4 < line.size()) {
                        unsigned cp = 0;
                        for (int i = 1; i <= 4; ++i) {
                            char h = line[p + i];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else cp = 0xFFFD;
                        }
                        p += 4;
                        if (cp < 0x80) *out += (char)cp;
                        else if (cp < 0x800) {
                            *out += (char)(0xC0 | (cp >> 6));
                            *out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            *out += (char)(0xE0 | (cp >> 12));
                            *out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            *out += (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: *out += e; break;
            }
            ++p;
        } else {
            *out += c;
            ++p;
        }
    }
    return false;  // 没等到收尾引号
}

}  // namespace vl

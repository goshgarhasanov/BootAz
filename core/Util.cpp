#include "Util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace bootaz {

std::string humanBytes(uint64_t bytes)
{
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    if (u == 0)
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else
        std::snprintf(buf, sizeof buf, v >= 100 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s)
{
    for (char& c : s)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && (std::isspace(static_cast<unsigned char>(s[b - 1])) || s[b - 1] == '\0'))
        --b;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& x)
{
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}
bool iequals(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty())
        return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string format(const std::string& fmt, const std::vector<std::string>& args)
{
    std::string out;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i + 1]))) {
            const size_t n = size_t(fmt[i + 1] - '1');
            if (n < args.size()) {
                out += args[n];
                ++i;
                continue;
            }
        }
        out += fmt[i];
    }
    return out;
}

std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                o += buf;
            } else {
                o += char(c);
            }
        }
    }
    return o;
}

std::string readTextFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool fileExists(const std::string& path)
{
    std::ifstream f(path);
    return bool(f);
}

std::u16string utf8ToUtf16(const std::string& s)
{
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
        else { cp = 0xfffd; n = 1; }
        if (i + n > s.size()) { cp = 0xfffd; n = 1; }
        for (int k = 1; k < n && cp != 0xfffd; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xd800 + (cp >> 10));
            out += char16_t(0xdc00 + (cp & 0x3ff));
        } else {
            out += char16_t(cp);
        }
    }
    return out;
}

std::string utf16ToUtf8(const std::u16string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = s[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (s[i + 1] - 0xdc00);
            ++i;
        }
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xc0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += char(0xe0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        } else {
            out += char(0xf0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3f));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

void JobContext::log(const std::string& line) const
{
    if (onLog)
        onLog(line);
}

void JobContext::progress(const std::string& stage, uint64_t done, uint64_t total) const
{
    if (onProgress)
        onProgress(Progress{stage, done, total, 0});
}

void JobContext::checkCancel() const
{
    if (cancel && cancel->load())
        throw Cancelled();
}

} // namespace bootaz

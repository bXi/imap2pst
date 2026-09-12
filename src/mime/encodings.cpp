#include "mime/encodings.h"

#include <cstdint>
#include <algorithm>
#include <cctype>

namespace imap2pst::mime {
namespace {

int base64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int hexValue(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

std::string decodeBase64(const std::string& input) {
    std::string out;
    out.reserve(input.size() * 3 / 4 + 3);
    std::uint32_t bits = 0;
    int have = 0;
    for (unsigned char c : input) {
        if (c == '=') break;              // padding: nothing follows that matters
        const int v = base64Value(c);
        if (v < 0) continue;              // whitespace, newlines, or junk
        bits = (bits << 6) | static_cast<std::uint32_t>(v);
        have += 6;
        if (have >= 8) {
            have -= 8;
            out.push_back(static_cast<char>((bits >> have) & 0xFF));
        }
    }
    return out;
}

std::string decodeQuotedPrintable(const std::string& input, bool underscore_is_space) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '_' && underscore_is_space) {
            out.push_back(' ');
            continue;
        }
        if (c != '=') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= input.size()) break;          // trailing '=' : drop it
        if (input[i + 1] == '\n') { i += 1; continue; }   // soft line break
        if (input[i + 1] == '\r') {
            i += (i + 2 < input.size() && input[i + 2] == '\n') ? 2 : 1;
            continue;
        }
        if (i + 2 < input.size()) {
            const int hi = hexValue(static_cast<unsigned char>(input[i + 1]));
            const int lo = hexValue(static_cast<unsigned char>(input[i + 2]));
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        // Not a valid escape.  Keep the '=' rather than swallowing it: some
        // senders emit bare '=' and losing it corrupts the text.
        out.push_back('=');
    }
    return out;
}

std::string decodeBody(const std::string& body, const std::string& encoding) {
    const std::string e = lower(encoding);
    if (e == "base64") return decodeBase64(body);
    if (e == "quoted-printable") return decodeQuotedPrintable(body, false);
    return body;
}

}  // namespace imap2pst::mime

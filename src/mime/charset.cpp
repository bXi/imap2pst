#include "mime/charset.h"

#include <unicode/ucnv.h>

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>

namespace imap2pst::mime {
namespace {

std::string trimLower(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (std::isspace(static_cast<unsigned char>(s[b])) || s[b] == '"')) ++b;
    while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])) || s[e - 1] == '"')) --e;
    std::string out = s.substr(b, e - b);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void appendUtf8(std::string* out, std::uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// The 32 positions where windows-1252 differs from ISO-8859-1.  Mail labelled
// iso-8859-1 very often contains these -- curly quotes and dashes from Word --
// so decoding as windows-1252 loses nothing and recovers those.
const std::uint16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

}  // namespace

bool isValidUtf8(const std::string& bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        std::size_t extra = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else return false;
        if (i + extra >= bytes.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(bytes[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Reject over-long forms and surrogates: they are how invalid input
        // sneaks past a naive validator.
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

std::string windows1252ToUtf8(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0xA0) {
            appendUtf8(&out, kCp1252High[c - 0x80]);
        } else {
            appendUtf8(&out, c);
        }
    }
    return out;
}

std::string toUtf8(const std::string& input, const std::string& charset) {
    if (input.empty()) return {};
    const std::string name = trimLower(charset);

    // Already UTF-8, or claims to be: believe it only if the bytes agree.
    if (name.empty() || name == "utf-8" || name == "utf8" || name == "us-ascii" ||
        name == "ascii" || name == "ansi_x3.4-1968" || name == "default") {
        if (isValidUtf8(input)) return input;
        // Declared ASCII or UTF-8 but is neither.  windows-1252 is the usual
        // truth, and it maps every byte, so nothing is lost.
        return windows1252ToUtf8(input);
    }
    if (name == "windows-1252" || name == "cp1252" || name == "iso-8859-1" ||
        name == "latin1" || name == "iso8859-1" || name == "8859-1") {
        // ISO-8859-1 is decoded as windows-1252 deliberately: mail labelled the
        // former routinely contains the latter's curly quotes and dashes, and
        // the two agree everywhere else.
        return windows1252ToUtf8(input);
    }

    UErrorCode status = U_ZERO_ERROR;
    UConverter* conv = ucnv_open(name.c_str(), &status);
    if (U_FAILURE(status) || conv == nullptr) {
        if (conv) ucnv_close(conv);
        return isValidUtf8(input) ? input : windows1252ToUtf8(input);
    }
    // Substitute rather than stop on malformed input: a message with one bad
    // byte should arrive with one replacement character, not fail to arrive.
    ucnv_setToUCallBack(conv, UCNV_TO_U_CALLBACK_SUBSTITUTE, nullptr, nullptr, nullptr,
                        &status);

    std::vector<char> out(input.size() * 4 + 16);
    status = U_ZERO_ERROR;
    const int32_t written = ucnv_toAlgorithmic(
        UCNV_UTF8, conv, out.data(), static_cast<int32_t>(out.size()), input.data(),
        static_cast<int32_t>(input.size()), &status);
    ucnv_close(conv);
    if (U_FAILURE(status) || written < 0) {
        return isValidUtf8(input) ? input : windows1252ToUtf8(input);
    }
    return std::string(out.data(), static_cast<std::size_t>(written));
}

}  // namespace imap2pst::mime

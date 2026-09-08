#include "imap/imap_parse.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

namespace imap2pst::imap {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void skipSpace(const std::string& s, std::size_t* i) {
    while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\t')) ++*i;
}

// Reads a quoted-string, a literal introduced by {n}, or a bare atom.
bool readAstring(const std::string& s, std::size_t* i, std::string* out) {
    skipSpace(s, i);
    if (*i >= s.size()) return false;
    if (s[*i] == '"') {
        ++*i;
        out->clear();
        while (*i < s.size() && s[*i] != '"') {
            if (s[*i] == '\\' && *i + 1 < s.size()) ++*i;
            out->push_back(s[(*i)++]);
        }
        if (*i < s.size()) ++*i;  // closing quote
        return true;
    }
    if (s[*i] == '{') {
        const std::size_t close = s.find('}', *i);
        if (close == std::string::npos) return false;
        const std::size_t n = std::strtoul(s.c_str() + *i + 1, nullptr, 10);
        std::size_t start = close + 1;
        if (start < s.size() && s[start] == '\r') ++start;
        if (start < s.size() && s[start] == '\n') ++start;
        if (start + n > s.size()) return false;
        *out = s.substr(start, n);
        *i = start + n;
        return true;
    }
    const std::size_t start = *i;
    while (*i < s.size() && s[*i] != ' ' && s[*i] != '\r' && s[*i] != '\n' &&
           s[*i] != '(' && s[*i] != ')') {
        ++*i;
    }
    if (*i == start) return false;
    *out = s.substr(start, *i - start);
    return true;
}

// Splits a response into lines, keeping literal payloads attached to the line
// that introduced them.
std::vector<std::string> splitLines(const std::string& response) {
    std::vector<std::string> lines;
    std::size_t i = 0;
    while (i < response.size()) {
        std::size_t eol = response.find('\n', i);
        if (eol == std::string::npos) eol = response.size();
        std::string line = response.substr(i, eol - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        i = eol + 1;
        // A trailing {n} means the next n bytes belong to this line.
        while (!line.empty() && line.back() == '}') {
            const std::size_t brace = line.rfind('{');
            if (brace == std::string::npos) break;
            const std::string digits = line.substr(brace + 1, line.size() - brace - 2);
            if (digits.empty() ||
                !std::all_of(digits.begin(), digits.end(),
                             [](unsigned char c) { return std::isdigit(c) != 0; })) {
                break;
            }
            const std::size_t n = std::strtoul(digits.c_str(), nullptr, 10);
            if (i + n > response.size()) break;
            line += "\r\n";
            line += response.substr(i, n);
            i += n;
            std::size_t rest_eol = response.find('\n', i);
            if (rest_eol == std::string::npos) rest_eol = response.size();
            std::string rest = response.substr(i, rest_eol - i);
            if (!rest.empty() && rest.back() == '\r') rest.pop_back();
            line += rest;
            i = rest_eol + 1;
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace

std::uint32_t parseFlags(const std::string& flag_list,
                         std::vector<std::string>* keywords) {
    std::uint32_t flags = kFlagNone;
    std::istringstream is(flag_list);
    std::string tok;
    while (is >> tok) {
        while (!tok.empty() && (tok.front() == '(' )) tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ')')) tok.pop_back();
        if (tok.empty()) continue;
        const std::string l = lower(tok);
        if (l == "\\seen") flags |= kFlagSeen;
        else if (l == "\\answered") flags |= kFlagAnswered;
        else if (l == "\\flagged") flags |= kFlagFlagged;
        else if (l == "\\deleted") flags |= kFlagDeleted;
        else if (l == "\\draft") flags |= kFlagDraft;
        else if (l == "\\recent") flags |= kFlagRecent;
        else if (keywords) keywords->push_back(tok);
    }
    return flags;
}

std::int64_t parseInternalDate(const std::string& value) {
    static const char* kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    char mon[8] = {};
    char sign = '+';
    int zone_hh = 0, zone_mm = 0;
    // "17-Jul-1996 02:44:25 -0700", optionally with a leading space for 1-digit
    // days ("  2-Jan-2020 ...").
    const int matched = std::sscanf(value.c_str(), " %d-%3s-%d %d:%d:%d %c%2d%2d",
                                    &day, mon, &year, &hour, &minute, &second,
                                    &sign, &zone_hh, &zone_mm);
    if (matched < 6) return 0;

    int month = -1;
    const std::string m = lower(mon);
    for (int i = 0; i < 12; ++i) {
        if (m == kMonths[i]) { month = i; break; }
    }
    if (month < 0) return 0;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    const std::time_t utc = timegm(&tm);
    if (utc == static_cast<std::time_t>(-1)) return 0;

    std::int64_t offset = 0;
    if (matched >= 9) {
        offset = (zone_hh * 60 + zone_mm) * 60;
        if (sign == '-') offset = -offset;
    }
    return static_cast<std::int64_t>(utc) - offset;
}

std::vector<FolderInfo> parseListResponse(const std::string& response) {
    std::vector<FolderInfo> out;
    for (const auto& line : splitLines(response)) {
        if (line.size() < 2 || line[0] != '*') continue;
        std::size_t i = 1;
        skipSpace(line, &i);
        if (lower(line.substr(i, 4)) != "list") continue;
        i += 4;
        skipSpace(line, &i);
        if (i >= line.size() || line[i] != '(') continue;

        const std::size_t close = line.find(')', i);
        if (close == std::string::npos) continue;
        const std::string attrs = lower(line.substr(i, close - i + 1));
        i = close + 1;

        FolderInfo info;
        info.selectable = attrs.find("\\noselect") == std::string::npos;

        std::string delim;
        if (!readAstring(line, &i, &delim)) continue;
        info.delimiter = (lower(delim) == "nil" || delim.empty()) ? '\0' : delim[0];

        if (!readAstring(line, &i, &info.full_name)) continue;
        if (info.full_name.empty()) continue;
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<MessageMeta> parseFetchResponse(const std::string& response) {
    std::vector<MessageMeta> out;
    for (const auto& line : splitLines(response)) {
        if (line.empty() || line[0] != '*') continue;
        const std::size_t fetch = lower(line).find(" fetch ");
        if (fetch == std::string::npos) continue;

        MessageMeta meta;
        bool have_uid = false;
        std::size_t i = fetch + 7;
        while (i < line.size()) {
            skipSpace(line, &i);
            if (i >= line.size()) break;
            if (line[i] == '(' || line[i] == ')') { ++i; continue; }
            std::string key;
            if (!readAstring(line, &i, &key)) break;
            const std::string k = lower(key);
            if (k == "uid") {
                std::string v;
                if (readAstring(line, &i, &v)) {
                    meta.uid = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    have_uid = true;
                }
            } else if (k == "rfc822.size") {
                std::string v;
                if (readAstring(line, &i, &v)) {
                    meta.size = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                }
            } else if (k == "flags") {
                skipSpace(line, &i);
                const std::size_t close = line.find(')', i);
                if (close == std::string::npos) break;
                meta.flags = parseFlags(line.substr(i, close - i + 1), &meta.keywords);
                i = close + 1;
            } else if (k == "internaldate") {
                std::string v;
                if (readAstring(line, &i, &v)) meta.internal_date = parseInternalDate(v);
            }
        }
        if (have_uid) out.push_back(std::move(meta));
    }
    return out;
}

}  // namespace imap2pst::imap

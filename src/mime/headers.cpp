#include "mime/headers.h"

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <map>

#include "mime/charset.h"
#include "mime/encodings.h"

namespace imap2pst::mime {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Strips surrounding double quotes and undoes backslash escapes inside them.
std::string unquote(const std::string& s) {
    const std::string t = trim(s);
    if (t.size() < 2 || t.front() != '"' || t.back() != '"') return t;
    std::string out;
    for (std::size_t i = 1; i + 1 < t.size(); ++i) {
        if (t[i] == '\\' && i + 2 < t.size()) ++i;
        out.push_back(t[i]);
    }
    return out;
}

// Splits on `sep`, honouring quoted strings, parenthesised comments and angle
// brackets, so a separator inside any of them does not split.
std::vector<std::string> splitOutsideQuotes(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string current;
    bool in_quotes = false, in_angle = false;
    int comment_depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (in_quotes) {
            if (c == '\\' && i + 1 < s.size()) { current.push_back(c); current.push_back(s[++i]); continue; }
            if (c == '"') in_quotes = false;
            current.push_back(c);
            continue;
        }
        if (comment_depth > 0) {
            if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
            if (c == '(') ++comment_depth;
            if (c == ')') --comment_depth;
            continue;   // comments are not part of the value
        }
        switch (c) {
            case '"': in_quotes = true; current.push_back(c); continue;
            case '(': ++comment_depth; continue;
            case '<': in_angle = true; current.push_back(c); continue;
            case '>': in_angle = false; current.push_back(c); continue;
            default: break;
        }
        if (c == sep && !in_angle) {
            out.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    out.push_back(current);
    return out;
}

bool isEncodedWordStart(const std::string& s, std::size_t i) {
    return i + 1 < s.size() && s[i] == '=' && s[i + 1] == '?';
}

// Decodes one "=?charset?enc?text?=" at `i`.  Returns false when it is not a
// well-formed encoded word, in which case nothing is consumed.
bool decodeOneWord(const std::string& s, std::size_t i, std::string* out,
                   std::size_t* consumed) {
    const std::size_t charset_end = s.find('?', i + 2);
    if (charset_end == std::string::npos) return false;
    const std::size_t enc_end = s.find('?', charset_end + 1);
    if (enc_end == std::string::npos || enc_end != charset_end + 2) return false;
    const std::size_t text_end = s.find("?=", enc_end + 1);
    if (text_end == std::string::npos) return false;

    std::string charset = s.substr(i + 2, charset_end - i - 2);
    // RFC 2231 allows a language tag: "utf-8*en".
    const std::size_t star = charset.find('*');
    if (star != std::string::npos) charset = charset.substr(0, star);

    const char encoding = static_cast<char>(std::toupper(static_cast<unsigned char>(s[charset_end + 1])));
    const std::string text = s.substr(enc_end + 1, text_end - enc_end - 1);

    std::string decoded;
    if (encoding == 'B') {
        decoded = decodeBase64(text);
    } else if (encoding == 'Q') {
        decoded = decodeQuotedPrintable(text, /*underscore_is_space=*/true);
    } else {
        return false;
    }
    *out = toUtf8(decoded, charset);
    *consumed = text_end + 2 - i;
    return true;
}

}  // namespace

std::vector<HeaderField> parseHeaderBlock(const std::string& source,
                                          std::size_t* body_offset) {
    std::vector<HeaderField> out;
    std::size_t i = 0;
    std::string name, value;
    bool have = false;
    auto flush = [&]() {
        if (have) out.push_back({name, value});
        have = false;
        name.clear();
        value.clear();
    };

    while (i < source.size()) {
        std::size_t eol = source.find('\n', i);
        const bool last = eol == std::string::npos;
        if (last) eol = source.size();
        std::string line = source.substr(i, eol - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t next = last ? source.size() : eol + 1;

        if (line.empty()) {            // blank line: the body starts after it
            i = next;
            break;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            // Folded continuation.  The fold itself becomes a single space,
            // which is what unfolding means; the RFC 2047 rules that care about
            // whitespace between encoded words are applied when decoding.
            if (have) {
                value += ' ';
                value += trim(line);
            }
            i = next;
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            // Not a header line at all.  Treat it as the start of the body:
            // some mail has no blank line before its content.
            break;
        }
        flush();
        name = trim(line.substr(0, colon));
        value = trim(line.substr(colon + 1));
        have = true;
        i = next;
    }
    flush();
    if (body_offset) *body_offset = i;
    return out;
}

std::string headerValue(const std::vector<HeaderField>& fields, const std::string& name) {
    const std::string want = lower(name);
    for (const auto& f : fields) {
        if (lower(f.name) == want) return f.value;
    }
    return {};
}

bool hasHeader(const std::vector<HeaderField>& fields, const std::string& name) {
    const std::string want = lower(name);
    return std::any_of(fields.begin(), fields.end(),
                       [&](const HeaderField& f) { return lower(f.name) == want; });
}

std::string decodeEncodedWords(const std::string& text) {
    std::string out;
    bool previous_was_encoded = false;
    std::size_t pending_space = 0;   // whitespace held back between words

    std::size_t i = 0;
    while (i < text.size()) {
        if (isEncodedWordStart(text, i)) {
            std::string decoded;
            std::size_t consumed = 0;
            if (decodeOneWord(text, i, &decoded, &consumed)) {
                // Whitespace between two encoded words is not part of the text.
                if (!previous_was_encoded) out.append(pending_space, ' ');
                pending_space = 0;
                out += decoded;
                previous_was_encoded = true;
                i += consumed;
                continue;
            }
        }
        if (std::isspace(static_cast<unsigned char>(text[i]))) {
            ++pending_space;
            ++i;
            continue;
        }
        out.append(pending_space, ' ');
        pending_space = 0;
        previous_was_encoded = false;
        out.push_back(text[i]);
        ++i;
    }
    out.append(pending_space, ' ');
    return out;
}

std::string fieldToken(const std::string& value) {
    const auto parts = splitOutsideQuotes(value, ';');
    return parts.empty() ? std::string() : trim(parts[0]);
}

std::string fieldParameter(const std::string& value, const std::string& name) {
    const std::string want = lower(name);
    const auto parts = splitOutsideQuotes(value, ';');

    // RFC 2231: a long parameter is split as name*0, name*1, ... and may carry
    // a charset as name*=charset'lang'text.  Collect the pieces first so the
    // continuations can be joined in order.
    std::map<int, std::string> continuations;
    std::string simple;
    bool extended = false;
    for (std::size_t i = 1; i < parts.size(); ++i) {
        const std::string part = trim(parts[i]);
        const std::size_t eq = part.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(part.substr(0, eq)));
        const std::string val = part.substr(eq + 1);
        bool this_extended = false;
        if (!key.empty() && key.back() == '*') {
            key.pop_back();
            this_extended = true;
        }
        int index = -1;
        const std::size_t star = key.find('*');
        if (star != std::string::npos) {
            index = std::atoi(key.c_str() + star + 1);
            key = key.substr(0, star);
        }
        if (key != want) continue;
        if (index >= 0) {
            continuations[index] = unquote(val);
            extended = extended || this_extended;
        } else {
            simple = unquote(val);
            extended = this_extended;
        }
    }

    std::string joined = simple;
    for (const auto& [index, piece] : continuations) {
        (void)index;
        joined += piece;
    }
    if (joined.empty()) return {};

    if (extended) {
        // charset'language'percent-encoded-text
        const std::size_t first = joined.find('\'');
        const std::size_t second = first == std::string::npos
                                       ? std::string::npos
                                       : joined.find('\'', first + 1);
        std::string charset, encoded = joined;
        if (second != std::string::npos) {
            charset = joined.substr(0, first);
            encoded = joined.substr(second + 1);
        }
        std::string decoded;
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] == '%' && i + 2 < encoded.size()) {
                const std::string hex = encoded.substr(i + 1, 2);
                decoded.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
                i += 2;
            } else {
                decoded.push_back(encoded[i]);
            }
        }
        return toUtf8(decoded, charset);
    }
    // A parameter may also carry encoded words, which is not legal but happens.
    return decodeEncodedWords(joined);
}

std::vector<Mailbox> parseAddressList(const std::string& value) {
    // Groups first: "Team: alice@x, bob@y;" holds commas of its own, so the
    // group name has to come off before the list is split on them.  A group
    // with no members -- "undisclosed-recipients:;" -- correctly yields none.
    std::string flattened;
    for (const auto& group : splitOutsideQuotes(value, ';')) {
        std::string part = trim(group);
        if (part.empty()) continue;
        const std::size_t colon = part.find(':');
        if (colon != std::string::npos) {
            const std::string label = part.substr(0, colon);
            // Only a group label, not a colon inside an address or a display
            // name, which would take the name with it.
            if (label.find('@') == std::string::npos &&
                label.find('<') == std::string::npos &&
                label.find('"') == std::string::npos) {
                part = trim(part.substr(colon + 1));
            }
        }
        if (part.empty()) continue;
        if (!flattened.empty()) flattened += ", ";
        flattened += part;
    }

    std::vector<Mailbox> out;
    for (const auto& raw : splitOutsideQuotes(flattened, ',')) {
        std::string part = trim(raw);
        if (part.empty()) continue;

        Mailbox mb;
        const std::size_t open = part.rfind('<');
        const std::size_t close = part.rfind('>');
        if (open != std::string::npos && close != std::string::npos && close > open) {
            mb.email = trim(part.substr(open + 1, close - open - 1));
            mb.name = decodeEncodedWords(unquote(trim(part.substr(0, open))));
        } else {
            mb.email = trim(part);
        }
        // An address that arrived without a domain keeps none.  Completing it
        // would put this machine's host name into someone else's mail.
        if (mb.email.empty() && mb.name.empty()) continue;
        if (mb.name == mb.email) mb.name.clear();
        out.push_back(std::move(mb));
    }
    return out;
}

}  // namespace imap2pst::mime

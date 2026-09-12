#include "mime/entity.h"

#include <algorithm>
#include <cctype>

#include "mime/charset.h"
#include "mime/encodings.h"

namespace imap2pst::mime {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string stripAngles(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    if (e > b && s[b] == '<' && s[e - 1] == '>') { ++b; --e; }
    return s.substr(b, e - b);
}

// Finds the next boundary line: "--boundary" at the start of a line.  Returns
// npos when there is none.  `is_final` is set when the line ends "--", which
// closes the multipart.
std::size_t findBoundary(const std::string& body, const std::string& boundary,
                         std::size_t from, std::size_t* line_end, bool* is_final) {
    const std::string marker = "--" + boundary;
    std::size_t i = from;
    while (i <= body.size()) {
        const std::size_t at = body.find(marker, i);
        if (at == std::string::npos) return std::string::npos;
        // Must be at the start of a line.
        const bool at_line_start = at == 0 || body[at - 1] == '\n';
        if (!at_line_start) {
            i = at + marker.size();
            continue;
        }
        std::size_t after = at + marker.size();
        *is_final = body.compare(after, 2, "--") == 0;
        if (*is_final) after += 2;
        // The rest of the line is transport padding and is ignored.
        std::size_t eol = body.find('\n', after);
        *line_end = eol == std::string::npos ? body.size() : eol + 1;
        return at;
    }
    return std::string::npos;
}

// Trims the CRLF that belongs to the boundary rather than to the part.
std::string trimTrailingLineEnd(const std::string& s) {
    std::size_t e = s.size();
    if (e > 0 && s[e - 1] == '\n') --e;
    if (e > 0 && s[e - 1] == '\r') --e;
    return s.substr(0, e);
}

}  // namespace

std::unique_ptr<Entity> parseEntity(const std::string& source, int depth) {
    auto entity = std::make_unique<Entity>();
    std::size_t body_offset = 0;
    entity->headers = parseHeaderBlock(source, &body_offset);
    entity->raw_body = source.substr(body_offset);

    const std::string content_type = headerValue(entity->headers, "Content-Type");
    const std::string media = lower(fieldToken(content_type));
    const std::size_t slash = media.find('/');
    if (slash == std::string::npos || media.empty()) {
        // No Content-Type means text/plain; us-ascii, per RFC 2045.
        entity->type = "text";
        entity->subtype = "plain";
    } else {
        entity->type = media.substr(0, slash);
        entity->subtype = media.substr(slash + 1);
    }
    entity->charset = fieldParameter(content_type, "charset");

    const std::string disposition = headerValue(entity->headers, "Content-Disposition");
    entity->disposition = lower(fieldToken(disposition));
    entity->filename = fieldParameter(disposition, "filename");
    if (entity->filename.empty()) {
        // Older senders put the name on Content-Type instead.
        entity->filename = fieldParameter(content_type, "name");
    }
    entity->content_id = stripAngles(headerValue(entity->headers, "Content-ID"));

    if (entity->isMultipart() && depth < kMaxNestingDepth) {
        const std::string boundary = fieldParameter(content_type, "boundary");
        if (!boundary.empty()) {
            std::size_t cursor = 0;
            bool started = false;
            while (cursor <= entity->raw_body.size()) {
                std::size_t line_end = 0;
                bool is_final = false;
                const std::size_t at =
                    findBoundary(entity->raw_body, boundary, cursor, &line_end, &is_final);
                if (at == std::string::npos) {
                    // No closing boundary.  Whatever is left is the last part:
                    // a truncated message should still yield what it has.
                    if (started && cursor < entity->raw_body.size()) {
                        entity->parts.push_back(
                            parseEntity(entity->raw_body.substr(cursor), depth + 1));
                    }
                    break;
                }
                if (started) {
                    entity->parts.push_back(parseEntity(
                        trimTrailingLineEnd(entity->raw_body.substr(cursor, at - cursor)),
                        depth + 1));
                }
                started = true;
                cursor = line_end;
                if (is_final) break;
            }
            return entity;
        }
        // A multipart with no boundary cannot be split.  Keep the body, so the
        // content is preserved even though the structure is lost.
    }

    if (entity->isMessage() && depth < kMaxNestingDepth) {
        entity->parts.push_back(parseEntity(entity->raw_body, depth + 1));
        return entity;
    }

    const std::string encoding = headerValue(entity->headers, "Content-Transfer-Encoding");
    entity->body = decodeBody(entity->raw_body, fieldToken(encoding));
    return entity;
}

}  // namespace imap2pst::mime

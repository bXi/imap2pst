#pragma once

// The MIME entity tree.
//
// An entity is a header block and a body; a multipart body is a list of
// entities, and a message/rfc822 body is a single one.  That is the whole
// structure -- everything else is interpretation, which happens a layer up.

#include <memory>
#include <string>
#include <vector>

#include "mime/headers.h"

namespace imap2pst::mime {

struct Entity {
    std::vector<HeaderField> headers;

    // "text/plain", lowercased, defaulting to text/plain when absent as the
    // RFC requires.
    std::string type;
    std::string subtype;
    std::string charset;        // from the Content-Type parameter, may be empty
    std::string disposition;    // "inline" or "attachment", lowercased
    std::string filename;       // from Content-Disposition, or Content-Type name
    std::string content_id;     // without angle brackets

    // Decoded body of a leaf entity.  Empty for multipart and message parts,
    // whose content is in `parts` instead.
    std::string body;

    // The body exactly as it arrived, undecoded: what an embedded message needs
    // so it can be parsed again, and what preserves an attachment nobody can
    // decode.
    std::string raw_body;

    std::vector<std::unique_ptr<Entity>> parts;

    bool isMultipart() const { return type == "multipart"; }
    bool isMessage() const { return type == "message" && subtype == "rfc822"; }
    std::string mediaType() const { return type + "/" + subtype; }
};

// Parses one entity, recursively.  `depth` guards against a message that nests
// without end; past the limit a part is kept whole rather than taken apart, so
// the content survives even when the structure is not walked.
std::unique_ptr<Entity> parseEntity(const std::string& source, int depth = 0);

// The nesting limit. Eight is far past anything legitimate: a forward of a
// forward of a forward is three.
constexpr int kMaxNestingDepth = 8;

}  // namespace imap2pst::mime

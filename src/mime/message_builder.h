#pragma once

// Turns a parsed entity tree into the normalized Message the rest of the tool
// speaks: which part is the body, which parts are attachments, what the
// structured headers mean, and what message class the whole thing is.

#include "core/message.h"
#include "mime/entity.h"

namespace imap2pst::mime {

// Builds a Message from an already-parsed tree.  `depth` bounds the recursion
// into embedded messages, matching the entity parser's own limit.
Message buildMessage(const Entity& root, int depth = 0);

// Seconds since the Unix epoch from an RFC 5322 Date header, or 0 when it
// cannot be read.  Exposed for its own tests: date formats in the wild vary
// more than any other header.
std::int64_t parseDate(const std::string& value);

}  // namespace imap2pst::mime

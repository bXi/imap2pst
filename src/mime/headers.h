#pragma once

// Header parsing: the field block, RFC 2047 encoded words, structured field
// parameters, and address lists.
//
// Everything here works on the bytes as they arrived.  Nothing is regenerated,
// reflowed or completed: a header that is stored is the one that was sent.

#include <string>
#include <vector>

#include "core/message.h"

namespace imap2pst::mime {

// One header field, unfolded, with the value exactly as received.
struct HeaderField {
    std::string name;
    std::string value;
};

// Splits the header block, unfolding continuation lines.  Stops at the blank
// line; `body_offset` receives the index just past it, or the input size when
// there is no body.
std::vector<HeaderField> parseHeaderBlock(const std::string& source,
                                          std::size_t* body_offset);

// Case-insensitive lookup of the first field with this name.  Returns an empty
// string when absent, which callers distinguish from present-and-empty with
// hasHeader() when it matters.
std::string headerValue(const std::vector<HeaderField>& fields, const std::string& name);
bool hasHeader(const std::vector<HeaderField>& fields, const std::string& name);

// Decodes RFC 2047 encoded words ("=?utf-8?B?...?=") to UTF-8, leaving the rest
// of the text alone.  Adjacent encoded words are joined without the whitespace
// between them, as the RFC requires, so a subject split across several does not
// gain spaces.
std::string decodeEncodedWords(const std::string& text);

// Decodes a header value for display: RFC 2047 encoded words, plus recovery of
// raw 8-bit bytes.  Headers are supposed to be ASCII with anything else encoded,
// but plenty of mail simply puts the bytes in, and they are almost always
// windows-1252.  The recovery happens before decoding, so encoded words -- which
// are ASCII -- are unaffected by it.
std::string headerText(const std::string& raw);

// The first token of a structured field: "text/plain" out of
// "text/plain; charset=utf-8".
std::string fieldToken(const std::string& value);

// A named parameter out of a structured field, unquoted, with RFC 2231
// continuations joined and any charset applied.  Empty when absent.
std::string fieldParameter(const std::string& value, const std::string& name);

// Parses an address list into mailboxes: display names decoded, groups
// flattened, comments dropped.  An address with no domain keeps none -- it is
// recorded as it arrived rather than completed with a local host name.
std::vector<Mailbox> parseAddressList(const std::string& value);

}  // namespace imap2pst::mime

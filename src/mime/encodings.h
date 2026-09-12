#pragma once

// Content-Transfer-Encoding decoders.
//
// Both are written to get something out of malformed input rather than to
// reject it: mail is decoded once, years after it was written, by which time
// nobody can fix the sender.

#include <string>

namespace imap2pst::mime {

// Decodes base64, ignoring anything outside the alphabet (line breaks, and the
// stray characters that mail gateways insert).  Truncated input yields as many
// whole bytes as it contains.
std::string decodeBase64(const std::string& input);

// Decodes quoted-printable.  `underscore_is_space` selects the RFC 2047 "Q"
// variant used inside encoded words, where "_" means a space; in a message body
// it does not.
std::string decodeQuotedPrintable(const std::string& input, bool underscore_is_space = false);

// Applies whichever of the two `encoding` names, or returns the input unchanged
// for 7bit, 8bit, binary and anything unrecognised.
std::string decodeBody(const std::string& body, const std::string& encoding);

}  // namespace imap2pst::mime

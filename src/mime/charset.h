#pragma once

// Charset conversion, ICU-backed.
//
// Everything above this layer speaks UTF-8.  Real mail names charsets that do
// not exist, lies about the ones that do, and often declares us-ascii for bytes
// that are plainly not, so the job here is as much recovery as conversion.

#include <string>

namespace imap2pst::mime {

// Converts `input` from `charset` to UTF-8.
//
// An unknown or unusable charset is not an error: the bytes are still mail, and
// dropping them would lose the message.  The fallback is UTF-8 when the bytes
// are valid UTF-8 and windows-1252 otherwise, which is what the bytes usually
// turn out to be -- windows-1252 maps every byte, so it cannot fail and cannot
// produce replacement characters.
std::string toUtf8(const std::string& input, const std::string& charset);

// True when `bytes` is well-formed UTF-8.  Used to decide whether a declared
// charset is worth believing.
bool isValidUtf8(const std::string& bytes);

// Decodes windows-1252, which every byte has a meaning in.  Exposed because it
// is the fallback for anything that cannot be identified.
std::string windows1252ToUtf8(const std::string& input);

}  // namespace imap2pst::mime

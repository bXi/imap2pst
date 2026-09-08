#pragma once

// Parsers for the handful of IMAP server responses this tool consumes.
//
// These are deliberately free functions over strings so the test suite can feed
// them recorded responses without any network or libcurl involvement.

#include <cstdint>
#include <string>
#include <vector>

#include "core/message.h"

namespace imap2pst::imap {

// Per-message metadata from a "UID FETCH ... (FLAGS INTERNALDATE RFC822.SIZE)".
struct MessageMeta {
    std::uint32_t uid = 0;
    std::uint32_t flags = kFlagNone;
    std::vector<std::string> keywords;
    std::int64_t internal_date = 0;
    std::uint32_t size = 0;
};

// Parses the untagged "* LIST (...) "delim" name" lines of a LIST response.
std::vector<FolderInfo> parseListResponse(const std::string& response);

// Parses the untagged "* n FETCH (...)" lines of a UID FETCH response.
std::vector<MessageMeta> parseFetchResponse(const std::string& response);

// "17-Jul-1996 02:44:25 -0700" -> seconds since the Unix epoch.  0 on failure.
std::int64_t parseInternalDate(const std::string& value);

// Maps an IMAP flag list to the MessageFlag bitmask plus leftover keywords.
std::uint32_t parseFlags(const std::string& flag_list,
                         std::vector<std::string>* keywords);

}  // namespace imap2pst::imap

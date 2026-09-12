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

// Collapses a sorted UID list into an IMAP sequence set: "1,2,3,7" -> "1:3,7".
// Keeping it short matters because a batched fetch names every UID it wants.
std::string uidSet(const std::vector<std::uint32_t>& uids);

// Parses the untagged "* LIST (...) "delim" name" lines of a LIST response.
std::vector<FolderInfo> parseListResponse(const std::string& response);

// Parses the untagged "* n FETCH (...)" lines of a UID FETCH response.
std::vector<MessageMeta> parseFetchResponse(const std::string& response);

// One message body from a batched "UID FETCH <set> (UID BODY.PEEK[])".
struct FetchedBody {
    std::uint32_t uid = 0;
    std::string rfc822;
};

// Parses the bodies out of a batched fetch.  A response that interleaves
// several messages yields one entry each, in the order the server sent them;
// entries whose UID or body is missing are skipped rather than guessed at.
std::vector<FetchedBody> parseFetchBodies(const std::string& response);

// Decodes an IMAP mailbox name from modified UTF-7 (RFC 3501 5.1.3) to UTF-8.
// Input that contains no shift sequence is returned unchanged.
std::string decodeModifiedUtf7(const std::string& name);

// "17-Jul-1996 02:44:25 -0700" -> seconds since the Unix epoch.  0 on failure.
std::int64_t parseInternalDate(const std::string& value);

// Maps an IMAP flag list to the MessageFlag bitmask plus leftover keywords.
std::uint32_t parseFlags(const std::string& flag_list,
                         std::vector<std::string>* keywords);

}  // namespace imap2pst::imap

#pragma once

// The normalized message representation that every module speaks.
//
// The IMAP module produces RawMessage (bytes + IMAP metadata), the MIME module
// turns that into Message, and the PST writer consumes Message.  Nothing in
// this header depends on libcurl, vmime or the PST format, so the modules stay
// independently testable.

#include <cstdint>
#include <string>
#include <vector>

namespace imap2pst {

// IMAP system flags, as a bitmask.  Keyword flags that are not one of the
// RFC 3501 system flags are kept verbatim in RawMessage::keywords.
enum MessageFlag : std::uint32_t {
    kFlagNone     = 0,
    kFlagSeen     = 1u << 0,
    kFlagAnswered = 1u << 1,
    kFlagFlagged  = 1u << 2,
    kFlagDeleted  = 1u << 3,
    kFlagDraft    = 1u << 4,
    kFlagRecent   = 1u << 5,
};

struct Mailbox {
    std::string name;   // display name, may be empty
    std::string email;  // addr-spec

    bool operator==(const Mailbox& o) const {
        return name == o.name && email == o.email;
    }
};

struct Attachment {
    std::string filename;      // best available name, may be empty
    std::string content_type;  // e.g. "application/pdf"
    std::string content_id;    // without angle brackets, may be empty
    bool is_inline = false;    // Content-Disposition: inline
    std::vector<std::uint8_t> data;
};

// A header that has no structured slot on Message.  Kept so nothing from the
// original RFC 822 headers is silently dropped.
struct RawHeader {
    std::string name;
    std::string value;
};

struct Message {
    // --- structured headers -------------------------------------------------
    Mailbox from;
    std::vector<Mailbox> to;
    std::vector<Mailbox> cc;
    std::vector<Mailbox> bcc;
    std::vector<Mailbox> reply_to;
    std::string subject;
    std::string message_id;   // without angle brackets
    std::string in_reply_to;

    // Seconds since the Unix epoch, from the Date: header.  0 when absent or
    // unparseable; use delivery_time in that case.
    std::int64_t date = 0;

    // --- bodies -------------------------------------------------------------
    // Both are UTF-8.  Either may be empty; a message with neither is legal.
    std::string body_text;
    std::string body_html;

    std::vector<Attachment> attachments;

    // Every header of the top-level part, in original order, including the ones
    // mirrored into the structured fields above.  This is what gets written to
    // PidTagTransportMessageHeaders.
    std::vector<RawHeader> headers;

    // --- IMAP-derived state -------------------------------------------------
    std::uint32_t flags = kFlagNone;
    std::vector<std::string> keywords;
    std::uint32_t imap_uid = 0;
    // Seconds since the Unix epoch, from IMAP INTERNALDATE.
    std::int64_t delivery_time = 0;
    // Octet count reported by IMAP, or the size of the raw source.
    std::uint32_t size = 0;

    bool seen() const { return (flags & kFlagSeen) != 0; }

    // Reconstructs the original header block, CRLF separated.
    std::string header_blob() const;
};

// A message as it came off the wire, before MIME parsing.
struct RawMessage {
    std::uint32_t uid = 0;
    std::uint32_t flags = kFlagNone;
    std::vector<std::string> keywords;
    std::int64_t internal_date = 0;
    std::string rfc822;  // full BODY[] octets
};

struct FolderInfo {
    // Server-reported name, e.g. "INBOX.Work.2024".
    std::string full_name;
    // Hierarchy delimiter reported by LIST, '\0' when the server reports NIL.
    char delimiter = '/';
    bool selectable = true;

    // full_name split on delimiter.  ["INBOX", "Work", "2024"]
    std::vector<std::string> path() const;
    // Last path component, i.e. what the folder is called in its parent.
    std::string leaf_name() const;
};

}  // namespace imap2pst

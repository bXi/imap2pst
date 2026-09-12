#include "mime/mime_parser.h"

#include "mime/entity.h"
#include "mime/message_builder.h"

namespace imap2pst::mime {

Message parse(const std::string& raw) {
    // Parsing is total: every input yields a Message.  A message that cannot be
    // understood still has headers worth keeping and bytes worth carrying, and
    // a migration that drops mail because a parser disliked it is worse than
    // one that carries it imperfectly.
    const auto root = parseEntity(raw);
    Message out = buildMessage(*root);
    out.size = static_cast<std::uint32_t>(raw.size());
    return out;
}

Message parse(const RawMessage& raw) {
    Message out = parse(raw.rfc822);
    out.flags = raw.flags;
    out.keywords = raw.keywords;
    out.imap_uid = raw.uid;
    out.delivery_time = raw.internal_date;
    if (out.date == 0) out.date = raw.internal_date;
    return out;
}

}  // namespace imap2pst::mime

#pragma once

// MIME normalization: raw RFC 822 octets in, core::Message out.
//
// The work is done by the layers beneath: entity.h walks the multipart tree,
// headers.h parses fields and addresses, encodings.h undoes transfer encodings
// and charset.h turns whatever the bytes are into UTF-8.  Everything this
// module produces is UTF-8, which is why the PST writer can hard-code
// PidTagInternetCodepage to 65001.
//
// Parsing is total: every input produces a Message, however damaged.

#include <stdexcept>
#include <string>

#include "core/message.h"

namespace imap2pst::mime {

class ParseError : public std::runtime_error {
 public:
    using std::runtime_error::runtime_error;
};

// Parses `raw` into a normalized message.  IMAP-side state (flags, UID,
// INTERNALDATE) is not touched; use the RawMessage overload for that.
Message parse(const std::string& raw);

// Parses and then overlays the IMAP metadata carried on `raw`.
Message parse(const RawMessage& raw);

}  // namespace imap2pst::mime

#include "mime/mime_parser.h"

#include <algorithm>
#include <ctime>
#include <sstream>

#include <vmime/vmime.hpp>
#include <vmime/contentTypeField.hpp>
#include <vmime/platforms/posix/posixHandler.hpp>

namespace imap2pst::mime {
namespace {

// A forwarded message that itself forwards a message is ordinary; a chain
// deeper than this is not worth the stack or the file size.
constexpr int kMaxEmbeddedDepth = 8;

// vmime completes an address with no domain -- "To: root" -- by appending the
// local machine's host name, which would write the name of whatever server ran
// the migration into the customer's mail.  The handler below gives it a fixed,
// reserved domain (RFC 2606) instead, and convertMailbox strips it again, so an
// address that arrived without a domain leaves without one.
const char* const kSyntheticDomain = "invalid";

class NeutralHostHandler : public vmime::platforms::posix::posixHandler {
 public:
    const vmime::string getHostName() const override { return kSyntheticDomain; }
};

void installNeutralHost() {
    static bool done = [] {
        vmime::platform::setHandler<NeutralHostHandler>();
        return true;
    }();
    (void)done;
}

const vmime::charset& utf8() {
    static const vmime::charset cs("utf-8");
    return cs;
}

std::string toUtf8(const std::string& in, const vmime::charset& from) {
    if (in.empty()) return in;
    try {
        std::string out;
        vmime::charset::convert(in, out, from, utf8());
        return out;
    } catch (const vmime::exception&) {
        // An undecodable body is better preserved verbatim than dropped.
        return in;
    }
}

std::string extract(const vmime::shared_ptr<const vmime::contentHandler>& ch) {
    if (!ch) return {};
    std::ostringstream oss;
    vmime::utility::outputStreamAdapter os(oss);
    ch->extract(os);
    return oss.str();
}

// True when `s` is well-formed UTF-8.  Used to rescue headers that carry raw
// 8-bit bytes with no declared charset.
bool isValidUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t extra;
        std::uint32_t cp;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
        else return false;
        if (i + extra >= s.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        // Reject overlong forms, surrogates and out-of-range code points.
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

// Converts a header text to UTF-8.
//
// RFC 5322 says an unencoded header is us-ascii, and vmime honours that: a word
// with no declared charset containing raw 8-bit bytes converts to one U+FFFD
// per byte, destroying it.  Plenty of real senders emit raw UTF-8 display names
// anyway, so a word that vmime has left as us-ascii but which holds 8-bit bytes
// is re-read as UTF-8 when it is valid UTF-8, and as windows-1252 otherwise --
// which cannot fail and keeps the bytes legible.
std::string textToUtf8(const vmime::text& t) {
    std::string out;
    try {
        for (std::size_t i = 0; i < t.getWordCount(); ++i) {
            const auto word = t.getWordAt(i);
            if (!word) continue;
            const std::string& buffer = word->getBuffer();
            const bool eight_bit =
                std::any_of(buffer.begin(), buffer.end(), [](char c) {
                    return static_cast<unsigned char>(c) >= 0x80;
                });
            vmime::charset cs = word->getCharset();
            if (eight_bit && cs == vmime::charset(vmime::charsets::US_ASCII)) {
                cs = isValidUtf8(buffer) ? vmime::charset("utf-8")
                                         : vmime::charset("windows-1252");
            }
            out += toUtf8(buffer, cs);
        }
        return out;
    } catch (const vmime::exception&) {
        return t.getWholeBuffer();
    }
}

Mailbox convertMailbox(const vmime::mailbox& mb) {
    Mailbox out;
    out.name = textToUtf8(mb.getName());
    out.email = mb.getEmail().toString();
    // Undo the domain vmime bolts onto an address that arrived without one.
    const std::string suffix = std::string("@") + kSyntheticDomain;
    if (out.email.size() > suffix.size() &&
        out.email.compare(out.email.size() - suffix.size(), suffix.size(), suffix) == 0) {
        out.email.erase(out.email.size() - suffix.size());
    }
    if (out.name == out.email) out.name.clear();
    return out;
}

std::vector<Mailbox> convertAddressList(const vmime::addressList& list) {
    std::vector<Mailbox> out;
    for (std::size_t i = 0; i < list.getAddressCount(); ++i) {
        const auto addr = list.getAddressAt(i);
        if (!addr) continue;
        if (addr->isGroup()) {
            const auto grp = vmime::dynamicCast<const vmime::mailboxGroup>(addr);
            if (!grp) continue;
            for (std::size_t k = 0; k < grp->getMailboxCount(); ++k) {
                out.push_back(convertMailbox(*grp->getMailboxAt(k)));
            }
        } else {
            const auto mb = vmime::dynamicCast<const vmime::mailbox>(addr);
            if (mb) out.push_back(convertMailbox(*mb));
        }
    }
    return out;
}

std::int64_t toUnix(const vmime::datetime& dt) {
    if (dt.getYear() < 1970) return 0;
    std::tm tm{};
    tm.tm_year = dt.getYear() - 1900;
    tm.tm_mon = dt.getMonth() - 1;
    tm.tm_mday = dt.getDay();
    tm.tm_hour = dt.getHour();
    tm.tm_min = dt.getMinute();
    tm.tm_sec = dt.getSecond();
    const std::time_t utc = timegm(&tm);
    if (utc == static_cast<std::time_t>(-1)) return 0;
    // vmime reports the zone as an offset in minutes east of UTC.
    return static_cast<std::int64_t>(utc) - dt.getZone() * 60;
}

std::string stripAngles(std::string s) {
    if (s.size() >= 2 && s.front() == '<' && s.back() == '>') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Every header of the top-level part, in wire order.  Values are decoded to
// UTF-8 where vmime parsed them as encoded text and generated verbatim
// otherwise, so nothing is lost.
// Header values come from the source text rather than from vmime's regenerated
// form.  Regenerating rewrites what it parsed -- an address with no domain comes
// back with one bolted on -- and these are meant to be what arrived.  Only
// RFC 2047 encoded words are decoded, which changes no structure.
std::vector<RawHeader> collectHeaders(const std::string& raw) {
    std::vector<RawHeader> out;
    std::size_t i = 0;
    std::string name, value;
    auto flush = [&]() {
        if (name.empty()) return;
        std::string decoded = value;
        try {
            if (decoded.find("=?") != std::string::npos) {
                const auto text = vmime::text::decodeAndUnfold(decoded);
                if (text) decoded = textToUtf8(*text);
            }
        } catch (const vmime::exception&) {
            // Keep the raw value: a header that will not decode is still data.
        }
        out.push_back({name, decoded});
        name.clear();
        value.clear();
    };
    while (i < raw.size()) {
        std::size_t eol = raw.find('\n', i);
        if (eol == std::string::npos) eol = raw.size();
        std::string line = raw.substr(i, eol - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        i = eol + 1;
        if (line.empty()) break;                 // end of the header block
        if (line[0] == ' ' || line[0] == '\t') {  // folded continuation
            const std::size_t at = line.find_first_not_of(" \t");
            if (!name.empty() && at != std::string::npos) {
                value += ' ';
                value += line.substr(at);
            }
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        flush();
        name = line.substr(0, colon);
        const std::size_t at = line.find_first_not_of(" \t", colon + 1);
        value = at == std::string::npos ? std::string() : line.substr(at);
    }
    flush();
    return out;
}

void collectTextParts(const vmime::messageParser& mp, Message* out) {
    for (std::size_t i = 0; i < mp.getTextPartCount(); ++i) {
        const auto part = mp.getTextPartAt(i);
        if (!part) continue;
        const std::string body = extract(part->getText());
        const std::string utf8_body = toUtf8(body, part->getCharset());
        if (part->getType().getSubType() == vmime::mediaTypes::TEXT_HTML) {
            if (out->body_html.empty()) out->body_html = utf8_body;
            const auto html = vmime::dynamicCast<const vmime::htmlTextPart>(part);
            if (html) {
                // vmime keeps a plain-text alternative alongside the HTML part.
                if (out->body_text.empty()) {
                    const std::string plain = extract(html->getPlainText());
                    if (!plain.empty()) out->body_text = toUtf8(plain, html->getCharset());
                }
                // Images referenced from the HTML by cid: are "embedded
                // objects" here, not attachments, so they never appear in
                // getAttachmentList().  They are still parts of the message and
                // have to be carried over, or every inline image in a real
                // mailbox is silently dropped.
                for (std::size_t k = 0; k < html->getObjectCount(); ++k) {
                    const auto obj = html->getObjectAt(k);
                    if (!obj) continue;
                    Attachment a;
                    a.is_inline = true;
                    a.content_id = stripAngles(obj->getId());
                    // vmime reports the id as "cid:xxx" when that is how the
                    // HTML referenced it.
                    if (a.content_id.compare(0, 4, "cid:") == 0) a.content_id.erase(0, 4);
                    a.content_type =
                        obj->getType().getType() + "/" + obj->getType().getSubType();
                    const std::string data = extract(obj->getData());
                    a.data.assign(data.begin(), data.end());
                    if (a.filename.empty()) {
                        a.filename = a.content_id.empty() ? ("inline" + std::to_string(k + 1))
                                                          : a.content_id;
                    }
                    out->attachments.push_back(std::move(a));
                }
            }
        } else if (out->body_text.empty()) {
            out->body_text = utf8_body;
        }
    }
}

// Maps the top-level Content-Type onto a MAPI message class.  Outlook keys a
// good deal of its behaviour off this: a delivery report shown as an ordinary
// note loses its report rendering, and a signed message shown as a note offers
// the signature as a file attachment instead of validating it.
std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

std::string messageClassFor(const vmime::header& hdr) {
    if (!hdr.hasField(vmime::fields::CONTENT_TYPE)) return {};
    const auto field = hdr.findField(vmime::fields::CONTENT_TYPE);
    const auto value = vmime::dynamicCast<const vmime::contentTypeField>(field);
    if (!value) return {};
    const auto type = lower(value->getValue()->generate());

    auto parameter = [&](const std::string& name) {
        const auto p = value->findParameter(name);
        return p ? lower(p->getValue().generate()) : std::string{};
    };

    if (type.compare(0, 16, "multipart/report") == 0) {
        const std::string report = parameter("report-type");
        if (report == "delivery-status") return "REPORT.IPM.Note.NDR";
        if (report == "disposition-notification") return "REPORT.IPM.Note.IPNRN";
        return {};
    }
    if (type.compare(0, 16, "multipart/signed") == 0) return "IPM.Note.SMIME.MultipartSigned";
    if (type.compare(0, 23, "application/pkcs7-mime") == 0 ||
        type.compare(0, 26, "application/x-pkcs7-mime") == 0) {
        return "IPM.Note.SMIME";
    }
    return {};
}

Message parseAtDepth(const std::string& raw, int depth);

// Turns every message/rfc822 attachment into a parsed Message hanging off the
// attachment.  `depth` stops a message that forwards itself -- or a deliberately
// nested one -- from recursing without bound; past the limit the part stays an
// ordinary attachment, which is lossless, just less convenient to read.
// Mail delivered by BCC carries no To, Cc or Bcc header, so a parse of the
// visible headers finds no recipient at all and the message reaches Outlook
// addressed to nobody.  The delivery headers still name who received it.  It
// goes in as a BCC recipient, which is what it was: recording it as To would
// claim a header the message never had.
void recoverHiddenRecipients(Message* out) {
    if (!out->to.empty() || !out->cc.empty() || !out->bcc.empty()) return;
    // In preference order: the one the delivering agent wrote last is the one
    // that named this mailbox.
    static const char* kDeliveryHeaders[] = {"delivered-to", "x-original-to", "envelope-to"};
    for (const char* wanted : kDeliveryHeaders) {
        for (const auto& h : out->headers) {
            if (lower(h.name) != wanted) continue;
            std::string address = h.value;
            const std::size_t start = address.find_first_not_of(" \t<");
            const std::size_t end = address.find_last_not_of(" \t>\r\n");
            if (start == std::string::npos) continue;
            address = address.substr(start, end - start + 1);
            if (address.empty() || address.find('@') == std::string::npos) continue;
            out->bcc.push_back({std::string(), address});
            return;
        }
    }
}

void parseEmbeddedMessages(Message* out, int depth) {
    if (depth >= kMaxEmbeddedDepth) return;
    for (auto& a : out->attachments) {
        if (lower(a.content_type) != "message/rfc822" || a.data.empty()) continue;
        auto inner = std::make_shared<Message>(parseAtDepth(
            std::string(a.data.begin(), a.data.end()), depth + 1));
        if (a.filename.empty()) {
            a.filename = inner->subject.empty() ? "Forwarded message" : inner->subject;
        }
        a.embedded = std::move(inner);
    }
}

void collectAttachments(const vmime::messageParser& mp, Message* out) {
    for (std::size_t i = 0; i < mp.getAttachmentCount(); ++i) {
        const auto att = mp.getAttachmentAt(i);
        if (!att) continue;
        Attachment a;
        try {
            a.filename = att->getName().getConvertedText(utf8());
        } catch (const vmime::exception&) {
            a.filename = att->getName().getBuffer();
        }
        a.content_type = att->getType().getType() + "/" + att->getType().getSubType();
        const std::string data = extract(att->getData());
        a.data.assign(data.begin(), data.end());

        // Content-Id and inline disposition come off the part's own header.
        if (const auto hdr = att->getHeader()) {
            if (hdr->hasField(vmime::fields::CONTENT_ID)) {
                a.content_id = stripAngles(
                    hdr->findField(vmime::fields::CONTENT_ID)->getValue()->generate());
            }
            if (hdr->hasField(vmime::fields::CONTENT_DISPOSITION)) {
                const auto disp = hdr->findField(vmime::fields::CONTENT_DISPOSITION)
                                      ->getValue()
                                      ->generate();
                a.is_inline = disp.compare(0, 6, "inline") == 0;
            }
        }
        out->attachments.push_back(std::move(a));
    }
}

// Used when vmime cannot make sense of the message at all: split the header
// block off by hand so the caller still gets something addressable.
Message fallbackParse(const std::string& raw) {
    Message out;
    const std::size_t split = raw.find("\r\n\r\n");
    const std::size_t split_lf = raw.find("\n\n");
    std::size_t header_end = std::string::npos;
    std::size_t body_start = raw.size();
    if (split != std::string::npos && (split_lf == std::string::npos || split < split_lf)) {
        header_end = split;
        body_start = split + 4;
    } else if (split_lf != std::string::npos) {
        header_end = split_lf;
        body_start = split_lf + 2;
    }
    const std::string head = raw.substr(0, header_end == std::string::npos ? raw.size() : header_end);

    std::istringstream is(head);
    std::string line, name, value;
    auto flush = [&]() {
        if (!name.empty()) out.headers.push_back({name, value});
        name.clear();
        value.clear();
    };
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            value += " ";
            value += line.substr(line.find_first_not_of(" \t"));
            continue;
        }
        flush();
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        name = line.substr(0, colon);
        std::size_t vs = line.find_first_not_of(" \t", colon + 1);
        value = (vs == std::string::npos) ? "" : line.substr(vs);
    }
    flush();

    if (body_start < raw.size()) out.body_text = raw.substr(body_start);
    for (const auto& h : out.headers) {
        if (h.name == "Subject") out.subject = h.value;
    }
    return out;
}

Message parseAtDepth(const std::string& raw, int depth) {
    installNeutralHost();
    Message out;
    try {
        auto msg = vmime::make_shared<vmime::message>();
        msg->parse(raw);
        vmime::messageParser mp(msg);

        out.from = convertMailbox(mp.getExpeditor());
        out.to = convertAddressList(mp.getRecipients());
        out.cc = convertAddressList(mp.getCopyRecipients());
        out.bcc = convertAddressList(mp.getBlindCopyRecipients());
        out.subject = textToUtf8(mp.getSubject());
        out.date = toUnix(mp.getDate());

        if (const auto hdr = msg->getHeader()) {
            out.headers = collectHeaders(raw);
            if (hdr->hasField(vmime::fields::MESSAGE_ID)) {
                out.message_id = stripAngles(
                    hdr->findField(vmime::fields::MESSAGE_ID)->getValue()->generate());
            }
            if (hdr->hasField(vmime::fields::IN_REPLY_TO)) {
                out.in_reply_to = stripAngles(
                    hdr->findField(vmime::fields::IN_REPLY_TO)->getValue()->generate());
            }
            if (hdr->hasField(vmime::fields::REPLY_TO)) {
                const auto v = hdr->findField(vmime::fields::REPLY_TO)->getValue();
                if (const auto mb = vmime::dynamicCast<const vmime::mailbox>(v)) {
                    out.reply_to.push_back(convertMailbox(*mb));
                } else if (const auto al = vmime::dynamicCast<const vmime::addressList>(v)) {
                    out.reply_to = convertAddressList(*al);
                }
            }
        }

        if (const auto hdr = msg->getHeader()) {
            out.message_class = messageClassFor(*hdr);
        }

        recoverHiddenRecipients(&out);

        collectTextParts(mp, &out);
        collectAttachments(mp, &out);
        parseEmbeddedMessages(&out, depth);
    } catch (const vmime::exception& e) {
        out = fallbackParse(raw);
        out.headers.push_back({"X-Imap2Pst-Parse-Error", e.what()});
    }
    out.size = static_cast<std::uint32_t>(raw.size());
    return out;
}

}  // namespace

Message parse(const std::string& raw) { return parseAtDepth(raw, 0); }

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

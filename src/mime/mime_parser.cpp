#include "mime/mime_parser.h"

#include <ctime>
#include <sstream>

#include <vmime/vmime.hpp>

namespace imap2pst::mime {
namespace {

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

std::string textToUtf8(const vmime::text& t) {
    try {
        return t.getConvertedText(utf8());
    } catch (const vmime::exception&) {
        return t.getWholeBuffer();
    }
}

Mailbox convertMailbox(const vmime::mailbox& mb) {
    Mailbox out;
    out.name = textToUtf8(mb.getName());
    out.email = mb.getEmail().toString();
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
std::vector<RawHeader> collectHeaders(const vmime::header& hdr) {
    std::vector<RawHeader> out;
    for (const auto& field : hdr.getFieldList()) {
        RawHeader h;
        h.name = field->getName();
        const auto value = field->getValue();
        if (!value) continue;
        if (const auto t = vmime::dynamicCast<const vmime::text>(value)) {
            h.value = textToUtf8(*t);
        } else {
            h.value = value->generate();
        }
        out.push_back(std::move(h));
    }
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
            // vmime keeps a plain-text alternative alongside the HTML part.
            const auto html = vmime::dynamicCast<const vmime::htmlTextPart>(part);
            if (html && out->body_text.empty()) {
                const std::string plain = extract(html->getPlainText());
                if (!plain.empty()) out->body_text = toUtf8(plain, html->getCharset());
            }
        } else if (out->body_text.empty()) {
            out->body_text = utf8_body;
        }
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

}  // namespace

Message parse(const std::string& raw) {
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
            out.headers = collectHeaders(*hdr);
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

        collectTextParts(mp, &out);
        collectAttachments(mp, &out);
    } catch (const vmime::exception& e) {
        out = fallbackParse(raw);
        out.headers.push_back({"X-Imap2Pst-Parse-Error", e.what()});
    }
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

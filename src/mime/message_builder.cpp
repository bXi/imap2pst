#include "mime/message_builder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "mime/charset.h"
#include "mime/headers.h"

namespace imap2pst::mime {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string stripAngles(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    if (e > b && s[b] == '<' && s[e - 1] == '>') { ++b; --e; }
    return s.substr(b, e - b);
}

// Which headers have a structured home on Message, and so should not also
// become a named property.  Everything else does.
bool isStructuredHeader(const std::string& name) {
    static const char* kKnown[] = {
        "from", "to", "cc", "bcc", "reply-to", "subject", "date",
        "message-id", "in-reply-to",
    };
    const std::string l = lower(name);
    for (const char* k : kKnown) {
        if (l == k) return true;
    }
    return false;
}

// A message class for the things Outlook renders differently.  Anything not
// recognised stays an ordinary note.
std::string messageClassFor(const Entity& root) {
    const std::string content_type = headerValue(root.headers, "Content-Type");
    const std::string media = lower(fieldToken(content_type));
    if (media == "multipart/report") {
        const std::string report = lower(fieldParameter(content_type, "report-type"));
        if (report == "delivery-status") return "REPORT.IPM.Note.NDR";
        if (report == "disposition-notification") return "REPORT.IPM.Note.IPNRN";
        return {};
    }
    if (media == "multipart/signed") return "IPM.Note.SMIME.MultipartSigned";
    if (media == "application/pkcs7-mime" || media == "application/x-pkcs7-mime") {
        return "IPM.Note.SMIME";
    }
    return {};
}

bool isAttachment(const Entity& e) {
    if (e.disposition == "attachment") return true;
    if (!e.filename.empty()) return true;
    // An inline part that is not text is still a file as far as the PST is
    // concerned; it is referenced from the HTML by its content id.
    if (e.type != "text" && e.type != "multipart" && !e.isMessage()) return true;
    return false;
}

void addAttachment(const Entity& e, Message* out, int depth);

// Walks a part, filling in bodies and attachments.  `alternative` marks a part
// under multipart/alternative, where the last readable representation wins.
void collect(const Entity& e, Message* out, int depth, bool in_alternative) {
    if (e.isMultipart()) {
        const bool alternative = e.subtype == "alternative";
        for (const auto& part : e.parts) {
            collect(*part, out, depth, alternative);
        }
        return;
    }
    if (e.isMessage()) {
        addAttachment(e, out, depth);
        return;
    }
    if (isAttachment(e)) {
        addAttachment(e, out, depth);
        return;
    }
    if (e.type == "text") {
        const std::string text = toUtf8(e.body, e.charset);
        if (e.subtype == "html") {
            if (out->body_html.empty() || in_alternative) out->body_html = text;
        } else {
            if (out->body_text.empty() || in_alternative) out->body_text = text;
        }
        return;
    }
    // Not text, not a recognised container: keep it rather than drop it.
    addAttachment(e, out, depth);
}

void addAttachment(const Entity& e, Message* out, int depth) {
    Attachment a;
    a.content_type = e.mediaType();
    a.content_id = e.content_id;
    a.is_inline = e.disposition == "inline" || (!e.content_id.empty() && e.disposition.empty());
    a.filename = e.filename;

    if (e.isMessage() && !e.parts.empty() && depth < kMaxNestingDepth) {
        // A forwarded message is stored as a message, not as a blob, so Outlook
        // opens it in place.  The source is kept too, so nothing is lost.
        Message inner = buildMessage(*e.parts[0], depth + 1);
        if (a.filename.empty()) {
            a.filename = inner.subject.empty() ? "Forwarded message" : inner.subject;
        }
        a.embedded = std::make_shared<Message>(std::move(inner));
        a.data.assign(e.raw_body.begin(), e.raw_body.end());
        out->attachments.push_back(std::move(a));
        return;
    }

    a.data.assign(e.body.begin(), e.body.end());
    if (a.filename.empty()) {
        a.filename = a.content_id.empty()
                         ? ("attachment" + std::to_string(out->attachments.size() + 1))
                         : a.content_id;
    }
    out->attachments.push_back(std::move(a));
}

// Mail delivered by BCC carries no To, Cc or Bcc header, so the visible headers
// name nobody.  The delivery headers still say who received it.
void recoverHiddenRecipients(const Entity& root, Message* out) {
    if (!out->to.empty() || !out->cc.empty() || !out->bcc.empty()) return;
    static const char* kDeliveryHeaders[] = {"Delivered-To", "X-Original-To", "Envelope-To"};
    for (const char* name : kDeliveryHeaders) {
        const std::string value = headerValue(root.headers, name);
        if (value.empty() || value.find('@') == std::string::npos) continue;
        // Recorded as BCC, which is what it was: calling it To would claim a
        // header the message never had.
        for (auto& mb : parseAddressList(value)) {
            out->bcc.push_back(std::move(mb));
        }
        if (!out->bcc.empty()) return;
    }
}

}  // namespace

std::int64_t parseDate(const std::string& value) {
    static const char* kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};
    // "Tue, 21 Nov 2023 09:15:00 +0100", with the day name optional and the
    // zone sometimes a name rather than an offset.
    std::string s = value;
    const std::size_t comma = s.find(',');
    if (comma != std::string::npos && comma < 5) s = s.substr(comma + 1);

    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    char month_name[16] = {};
    char zone[16] = {};
    const int matched = std::sscanf(s.c_str(), " %d %15s %d %d:%d:%d %15s", &day,
                                    month_name, &year, &hour, &minute, &second, zone);
    if (matched < 6) return 0;

    int month = -1;
    const std::string m = lower(std::string(month_name).substr(0, 3));
    for (int i = 0; i < 12; ++i) {
        if (m == kMonths[i]) { month = i; break; }
    }
    if (month < 0) return 0;
    if (year < 100) year += year < 70 ? 2000 : 1900;   // two-digit years, RFC 822

    std::tm tm {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    const std::time_t utc = timegm(&tm);
    if (utc == static_cast<std::time_t>(-1)) return 0;

    std::int64_t offset = 0;
    if (matched >= 7 && (zone[0] == '+' || zone[0] == '-')) {
        const int digits = std::atoi(zone + 1);
        offset = (digits / 100) * 3600 + (digits % 100) * 60;
        if (zone[0] == '-') offset = -offset;
    }
    return static_cast<std::int64_t>(utc) - offset;
}

Message buildMessage(const Entity& root, int depth) {
    Message out;

    for (const auto& h : root.headers) {
        // Stored as received; only encoded words are decoded, which changes no
        // structure.  Nothing is regenerated, so an address that arrived
        // without a domain is not completed with this machine's host name.
        out.headers.push_back({h.name, headerText(h.value)});
    }

    out.subject = headerText(headerValue(root.headers, "Subject"));
    out.message_id = stripAngles(headerValue(root.headers, "Message-ID"));
    out.in_reply_to = stripAngles(headerValue(root.headers, "In-Reply-To"));
    out.date = parseDate(headerValue(root.headers, "Date"));

    auto from = parseAddressList(headerValue(root.headers, "From"));
    if (!from.empty()) out.from = from.front();
    out.to = parseAddressList(headerValue(root.headers, "To"));
    out.cc = parseAddressList(headerValue(root.headers, "Cc"));
    out.bcc = parseAddressList(headerValue(root.headers, "Bcc"));
    out.reply_to = parseAddressList(headerValue(root.headers, "Reply-To"));

    out.message_class = messageClassFor(root);
    recoverHiddenRecipients(root, &out);

    collect(root, &out, depth, /*in_alternative=*/false);

    (void)isStructuredHeader;   // used by the PST layer; kept here for symmetry
    return out;
}

}  // namespace imap2pst::mime

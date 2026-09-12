// Writes a PST with known content plus a JSON manifest describing exactly what
// went in.  tests/scripts/verify_pst.py reads the PST back with libpff and
// checks it against the manifest, so the oracle never shares code with the
// writer under test.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <memory>
#include <vector>

#include "pst/pst_writer.h"

using namespace imap2pst;

namespace {

std::string jsonEscape(const std::string& s) {
    std::ostringstream os;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) {
                    os << "\\u" << std::hex << std::uppercase;
                    os.width(4);
                    os.fill('0');
                    os << static_cast<int>(c) << std::dec << std::nouppercase;
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    return os.str();
}

std::string hexOf(const std::vector<std::uint8_t>& v) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(v.size() * 2);
    for (std::uint8_t b : v) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

struct Expected {
    std::string folder_path;  // slash separated, below "Top of Personal Folders"
    Message message;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: pst_fixture_writer OUT.pst OUT.json\n";
        return 2;
    }
    const std::string pst_path = argv[1];
    const std::string json_path = argv[2];

    std::vector<Expected> expected;

    auto make = [](const std::string& subject, const std::string& text,
                   const std::string& html, std::int64_t when) {
        Message m;
        m.subject = subject;
        m.from = {"Alice Example", "alice@example.com"};
        m.to = {{"Bob Builder", "bob@example.com"}};
        m.cc = {{"Carol", "carol@example.com"}};
        m.body_text = text;
        m.body_html = html;
        m.delivery_time = when;
        m.date = when;
        m.flags = kFlagSeen;
        m.headers = {{"Subject", subject},
                     {"From", "Alice Example <alice@example.com>"},
                     {"X-Fixture", "yes"},
                     {"X-Sequence", subject}};
        return m;
    };

    pst::PstWriter writer(pst_path);
    const auto inbox = writer.createFolder(writer.ipmSubtree(), "Inbox");
    const auto nested = writer.createFolder(inbox, "Nested Folder");
    const auto unicode_folder =
        writer.createFolder(writer.ipmSubtree(), "\xC3\x9C\x62\x65rsicht");

    {
        Message m = make("Plain message", "Just a plain body.\n", {}, 1700000000);
        writer.addMessage(inbox, m);
        expected.push_back({"Inbox", m});
    }
    {
        Message m = make("HTML message", "Plain alternative.",
                         "<html><body><p>Rich &amp; fancy</p></body></html>", 1700000100);
        writer.addMessage(inbox, m);
        expected.push_back({"Inbox", m});
    }
    {
        // Unicode subject and body, to prove the UTF-8 -> UTF-16 path.
        Message m = make("\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 \xC3\xA9\xC3\xA8",
                         "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\n",
                         {}, 1700000200);
        writer.addMessage(nested, m);
        expected.push_back({"Inbox/Nested Folder", m});
    }
    {
        Message m = make("With attachments", "See attached.", {}, 1700000300);
        Attachment a;
        a.filename = "hello.txt";
        a.content_type = "text/plain";
        const std::string payload = "attachment contents\n";
        a.data.assign(payload.begin(), payload.end());
        m.attachments.push_back(a);

        Attachment b;
        b.filename = "blob.bin";
        b.content_type = "application/octet-stream";
        // Non-ASCII bytes, so a naive text round-trip would corrupt them.
        for (int i = 0; i < 512; ++i) b.data.push_back(static_cast<std::uint8_t>(i * 7));
        m.attachments.push_back(b);

        writer.addMessage(nested, m);
        expected.push_back({"Inbox/Nested Folder", m});
    }
    {
        // Body and attachment large enough to need a multi-block data tree.
        Message m = make("Large payloads", std::string(300000, 'L'), {}, 1700000400);
        Attachment a;
        a.filename = "large.bin";
        a.content_type = "application/octet-stream";
        a.data.assign(1024 * 1024, 0x5A);
        m.attachments.push_back(a);
        writer.addMessage(unicode_folder, m);
        expected.push_back({"\xC3\x9C\x62\x65rsicht", m});
    }
    {
        // Everything the fidelity work added, in one message: a class other
        // than IPM.Note, IMAP keywords that become categories, the answered
        // flag, and a forwarded message carrying its own recipients and its own
        // attachment.
        Message m = make("Fwd: report", "Passing this on.", {}, 1700000500);
        m.message_class = "IPM.Note.SMIME.MultipartSigned";
        m.keywords = {"Work", "Urgent"};
        m.flags = kFlagSeen | kFlagAnswered;

        Message inner;
        inner.subject = "The original report";
        inner.from = {"Carol", "carol@example.org"};
        inner.to = {{"Alice Example", "alice@example.com"}};
        inner.body_text = "Numbers attached.";
        inner.delivery_time = 1700000450;
        inner.date = inner.delivery_time;
        Attachment csv;
        csv.filename = "numbers.csv";
        csv.content_type = "text/csv";
        const std::string csv_data = "quarter,revenue\nQ3,120\n";
        csv.data.assign(csv_data.begin(), csv_data.end());
        inner.attachments.push_back(csv);

        Attachment fwd;
        fwd.filename = "original.eml";
        fwd.content_type = "message/rfc822";
        const std::string src = "From: carol@example.org\r\nSubject: The original report\r\n";
        fwd.data.assign(src.begin(), src.end());
        fwd.embedded = std::make_shared<Message>(inner);
        m.attachments.push_back(fwd);

        writer.addMessage(inbox, m);
        expected.push_back({"Inbox", m});
    }
    // A folder big enough to force the streaming contents table across many
    // heap blocks and several row-matrix blocks, so the round-trip covers the
    // path that a real mailbox takes.
    const auto bulk = writer.createFolder(writer.ipmSubtree(), "Bulk");
    constexpr int kBulkCount = 3000;
    for (int i = 0; i < kBulkCount; ++i) {
        Message m = make("Bulk " + std::to_string(i),
                         "Body of bulk message " + std::to_string(i), {},
                         1700100000 + i);
        writer.addMessage(bulk, m);
    }
    writer.finish();

    std::ofstream js(json_path, std::ios::binary);
    js << "{\n  \"messages\": [\n";
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& e = expected[i];
        const auto& m = e.message;
        js << "    {\n"
           << "      \"folder\": \"" << jsonEscape(e.folder_path) << "\",\n"
           << "      \"subject\": \"" << jsonEscape(m.subject) << "\",\n"
           << "      \"sender_name\": \"" << jsonEscape(m.from.name) << "\",\n"
           << "      \"sender_email\": \"" << jsonEscape(m.from.email) << "\",\n"
           << "      \"display_to\": \"" << jsonEscape(m.to[0].name) << "\",\n"
           << "      \"body_text\": \"" << jsonEscape(m.body_text) << "\",\n"
           << "      \"body_html\": \"" << jsonEscape(m.body_html) << "\",\n"
           << "      \"delivery_time\": " << m.delivery_time << ",\n"
           << "      \"message_class\": \""
           << jsonEscape(m.message_class.empty() ? "IPM.Note" : m.message_class)
           << "\",\n"
           << "      \"categories\": [";
        for (std::size_t k = 0; k < m.keywords.size(); ++k) {
            js << (k ? ", " : "") << "\"" << jsonEscape(m.keywords[k]) << "\"";
        }
        js << "],\n"
           << "      \"wants_rtf\": " << (m.body_html.empty() && !m.body_text.empty() ? "true" : "false")
           << ",\n"

           << "      \"attachments\": [";
        for (std::size_t k = 0; k < m.attachments.size(); ++k) {
            const auto& a = m.attachments[k];
            js << (k ? ", " : "") << "{\"filename\": \"" << jsonEscape(a.filename) << "\"";
            if (a.embedded) {
                // An embedded message has no attachment payload of its own; it
                // is checked by opening it and looking at the message inside.
                js << ", \"embedded_subject\": \"" << jsonEscape(a.embedded->subject)
                   << "\", \"embedded_attachments\": " << a.embedded->attachments.size()
                   << ", \"embedded_body\": \"" << jsonEscape(a.embedded->body_text) << "\"";
            } else {
                js << ", \"size\": " << a.data.size() << ", \"data_hex\": \""
                   << hexOf(a.data) << "\"";
            }
            js << "}";
        }
        js << "]\n    }" << (i + 1 == expected.size() ? "\n" : ",\n");
    }
    js << "  ],\n"
       << "  \"folder_counts\": [{\"folder\": \"Bulk\", \"count\": " << kBulkCount
       << ", \"first_subject\": \"Bulk 0\", \"last_subject\": \"Bulk "
       << (kBulkCount - 1) << "\"}],\n"
       << "  \"named_properties\": [\"X-Fixture\", \"X-Sequence\"]\n}\n";
    if (!js) {
        std::cerr << "failed writing manifest\n";
        return 1;
    }
    std::cout << "wrote " << pst_path << " and " << json_path << "\n";
    return 0;
}

// MIME normalization against fixture .eml files.

#include <gtest/gtest.h>

#include <algorithm>

#include "mime/mime_parser.h"
#include "test_util.h"

namespace imap2pst::mime {
namespace {

bool hasHeader(const Message& m, const std::string& name) {
    return std::any_of(m.headers.begin(), m.headers.end(),
                       [&](const RawHeader& h) { return h.name == name; });
}

std::string headerValue(const Message& m, const std::string& name) {
    for (const auto& h : m.headers) {
        if (h.name == name) return h.value;
    }
    return {};
}

TEST(MimePlain, StructuredHeadersAndBody) {
    const Message m = parse(test::readFixture("plain.eml"));

    EXPECT_EQ(m.subject, "A plain text message");
    EXPECT_EQ(m.from.name, "Alice Example");
    EXPECT_EQ(m.from.email, "alice@example.com");
    ASSERT_EQ(m.to.size(), 1u);
    EXPECT_EQ(m.to[0].name, "Bob Builder");
    EXPECT_EQ(m.to[0].email, "bob@example.com");
    ASSERT_EQ(m.cc.size(), 1u);
    EXPECT_EQ(m.cc[0].email, "carol@example.com");
    EXPECT_EQ(m.message_id, "plain-001@example.com");
    EXPECT_EQ(m.date, 1700000000);

    EXPECT_NE(m.body_text.find("This is a plain text body."), std::string::npos);
    EXPECT_TRUE(m.body_html.empty());
    EXPECT_TRUE(m.attachments.empty());
}

TEST(MimePlain, KeepsHeadersWithoutAStructuredSlot) {
    const Message m = parse(test::readFixture("plain.eml"));
    // Nothing may be silently dropped: X-Mailer has no MAPI property of its own
    // but must still be present in the catch-all header list.
    EXPECT_TRUE(hasHeader(m, "X-Mailer"));
    EXPECT_EQ(headerValue(m, "X-Mailer"), "FixtureMailer 1.0");
    EXPECT_TRUE(hasHeader(m, "Subject"));
    EXPECT_NE(m.header_blob().find("X-Mailer: FixtureMailer 1.0"), std::string::npos);
}

TEST(MimeHtml, HtmlOnlyMessageHasNoPlainBody) {
    const Message m = parse(test::readFixture("html_only.eml"));
    EXPECT_EQ(m.subject, "HTML only newsletter");
    EXPECT_NE(m.body_html.find("<h1>Headline</h1>"), std::string::npos);
    EXPECT_TRUE(m.body_text.empty());
}

TEST(MimeMultipart, ExtractsAttachmentsWithBytesIntact) {
    const Message m = parse(test::readFixture("multipart_mixed.eml"));
    EXPECT_EQ(m.subject, "With attachments");
    EXPECT_NE(m.body_text.find("Please find two files attached."), std::string::npos);

    ASSERT_EQ(m.attachments.size(), 2u);

    const Attachment& bin = m.attachments[0];
    EXPECT_EQ(bin.filename, "data.bin");
    EXPECT_EQ(bin.content_type, "application/octet-stream");
    const std::string expected = "PDF-ish binary \x00\x01\x02\xff payload for the attachment test";
    const std::string actual(bin.data.begin(), bin.data.end());
    EXPECT_EQ(actual, std::string("PDF-ish binary ", 15) +
                          std::string("\x00\x01\x02\xff", 4) +
                          " payload for the attachment test");

    const Attachment& csv = m.attachments[1];
    EXPECT_EQ(csv.filename, "table.csv");
    EXPECT_EQ(csv.content_type, "text/csv");
    const std::string csv_text(csv.data.begin(), csv.data.end());
    EXPECT_NE(csv_text.find("1,2,3"), std::string::npos);
}

TEST(MimeCharset, Latin1HeadersAndBodiesBecomeUtf8) {
    const Message m = parse(test::readFixture("alternative_latin1.eml"));

    // "Grüße aus München" in UTF-8.
    EXPECT_EQ(m.subject, "Gr\xC3\xBC\xC3\x9F\x65 aus M\xC3\xBCnchen");
    EXPECT_EQ(m.from.name, "Andr\xC3\xA9 M\xC3\xBCller");
    EXPECT_EQ(m.from.email, "andre@example.de");

    EXPECT_NE(m.body_text.find("M\xC3\xBCnchen"), std::string::npos);
    EXPECT_NE(m.body_html.find("M\xC3\xBCnchen"), std::string::npos);
    // The latin-1 source bytes must not survive into the normalized message.
    EXPECT_EQ(m.body_text.find('\xFC'), std::string::npos);
}

TEST(MimeCharset, Utf8BodyAndEncodedWordSubject) {
    const Message m = parse(test::readFixture("utf8_body.eml"));
    EXPECT_EQ(m.subject, "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88\xE4\xBB\xB6\xE5\x90\x8D");
    EXPECT_NE(m.body_text.find("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"),
              std::string::npos);
}

TEST(MimeImapState, CarriesFlagsAndInternalDateThrough) {
    RawMessage raw;
    raw.uid = 4242;
    raw.flags = kFlagSeen | kFlagFlagged;
    raw.keywords = {"$Label1"};
    raw.internal_date = 1700000123;
    raw.rfc822 = test::readFixture("plain.eml");

    const Message m = parse(raw);
    EXPECT_EQ(m.imap_uid, 4242u);
    EXPECT_TRUE(m.seen());
    EXPECT_EQ(m.flags, kFlagSeen | kFlagFlagged);
    ASSERT_EQ(m.keywords.size(), 1u);
    EXPECT_EQ(m.delivery_time, 1700000123);
    // The Date: header still wins for `date`.
    EXPECT_EQ(m.date, 1700000000);
}

TEST(MimeRobustness, GarbageInputDoesNotThrow) {
    const Message m = parse("this is not a message at all");
    EXPECT_TRUE(m.subject.empty());
    EXPECT_GT(m.size, 0u);
}

}  // namespace
}  // namespace imap2pst::mime

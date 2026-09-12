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

TEST(MimeCharset, RawUtf8InUndeclaredHeadersSurvives) {
    // RFC 5322 says an unencoded header is us-ascii, so raw 8-bit bytes are
    // strictly non-conformant -- but real senders emit them constantly, and
    // treating each byte as an invalid us-ascii character turns the whole
    // display name into replacement characters.
    const Message m = parse(test::readFixture("raw8bit_headers.eml"));
    EXPECT_EQ(m.from.name, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E "
                           "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88");
    EXPECT_EQ(m.from.email, "jp@example.jp");
    EXPECT_EQ(m.subject, "\xE7\x94\x9F\xE3\x83\x87\xE3\x83\xBC\xE3\x82\xBF");
    EXPECT_EQ(m.from.name.find("\xEF\xBF\xBD"), std::string::npos)
        << "no byte should have decayed to U+FFFD";
}

TEST(MimeCharset, RawLatin1InUndeclaredHeadersFallsBack) {
    // Not valid UTF-8, so the windows-1252 fallback has to carry it.
    const Message m = parse(test::readFixture("raw8bit_latin1.eml"));
    EXPECT_EQ(m.from.name, "Andr\xC3\xA9 M\xC3\xBCller");
    EXPECT_EQ(m.subject, "Caf\xC3\xA9 r\xC3\xA9union");
    EXPECT_EQ(m.subject.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(MimeMultipart, InlineImagesReferencedByCidAreKept) {
    // vmime models a cid:-referenced image as an "embedded object" of the HTML
    // part rather than an attachment, so walking getAttachmentList() alone
    // drops every inline image in a real mailbox.
    const Message m = parse(test::readFixture("inline_image.eml"));
    ASSERT_EQ(m.attachments.size(), 2u);

    const Attachment* inlined = nullptr;
    const Attachment* regular = nullptr;
    for (const auto& a : m.attachments) {
        if (a.is_inline) inlined = &a;
        else regular = &a;
    }
    ASSERT_NE(inlined, nullptr) << "the inline image was dropped";
    ASSERT_NE(regular, nullptr);

    EXPECT_EQ(inlined->content_id, "pic@example.com");
    EXPECT_EQ(inlined->content_type, "image/png");
    ASSERT_GE(inlined->data.size(), 8u);
    // A PNG signature, so we know the payload survived transfer decoding.
    EXPECT_EQ(inlined->data[0], 0x89);
    EXPECT_EQ(inlined->data[1], 'P');
    EXPECT_EQ(inlined->data[2], 'N');
    EXPECT_EQ(inlined->data[3], 'G');

    EXPECT_EQ(regular->filename, "notes.txt");
    EXPECT_FALSE(regular->is_inline);
    EXPECT_NE(m.body_html.find("cid:pic@example.com"), std::string::npos);
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

TEST(Mime, ForwardedMessageIsParsedAsAnEmbeddedMessage) {
    const Message m = parse(test::readFixture("forwarded.eml"));
    EXPECT_EQ(m.subject, "Fwd: quarterly numbers");
    ASSERT_EQ(m.attachments.size(), 1u);

    const Attachment& a = m.attachments[0];
    EXPECT_EQ(a.content_type, "message/rfc822");
    ASSERT_TRUE(a.embedded) << "a message/rfc822 part must be parsed, not kept as a blob";
    // The original source is still there, so nothing is lost by embedding.
    EXPECT_FALSE(a.data.empty());

    const Message& inner = *a.embedded;
    EXPECT_EQ(inner.subject, "quarterly numbers");
    EXPECT_EQ(inner.from.email, "carol@example.org");
    ASSERT_EQ(inner.to.size(), 1u);
    EXPECT_EQ(inner.to[0].email, "alice@example.com");
    ASSERT_EQ(inner.cc.size(), 1u);
    EXPECT_EQ(inner.cc[0].email, "dave@example.org");
    EXPECT_NE(inner.body_text.find("Numbers attached."), std::string::npos);

    // The forwarded message keeps its own attachment.
    ASSERT_EQ(inner.attachments.size(), 1u);
    EXPECT_EQ(inner.attachments[0].filename, "numbers.csv");
}

TEST(Mime, MessageClassFollowsTheContentType) {
    // An ordinary message says nothing, and the writer defaults it.
    EXPECT_TRUE(parse(test::readFixture("plain.eml")).message_class.empty());

    const std::string ndr =
        "From: postmaster@example.com\r\n"
        "To: alice@example.com\r\n"
        "Subject: Undeliverable\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/report; report-type=delivery-status; boundary=b\r\n"
        "\r\n"
        "--b\r\nContent-Type: text/plain\r\n\r\nfailed\r\n"
        "--b\r\nContent-Type: message/delivery-status\r\n\r\nStatus: 5.1.1\r\n"
        "--b--\r\n";
    EXPECT_EQ(parse(ndr).message_class, "REPORT.IPM.Note.NDR");

    const std::string signed_mail =
        "From: alice@example.com\r\n"
        "To: bob@example.net\r\n"
        "Subject: signed\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; "
        "micalg=sha-256; boundary=s\r\n"
        "\r\n"
        "--s\r\nContent-Type: text/plain\r\n\r\nhello\r\n"
        "--s\r\nContent-Type: application/pkcs7-signature\r\n\r\nsig\r\n"
        "--s--\r\n";
    EXPECT_EQ(parse(signed_mail).message_class, "IPM.Note.SMIME.MultipartSigned");
}

}  // namespace
}  // namespace imap2pst::mime

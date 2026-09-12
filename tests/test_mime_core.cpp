// The MIME layer's foundations: transfer encodings, charset recovery and
// header parsing.  These are tested on their own because everything above them
// inherits their mistakes, and because the inputs that break them are the ones
// that never appear in well-formed mail.

#include <gtest/gtest.h>

#include <string>

#include "mime/charset.h"
#include "mime/encodings.h"
#include "mime/entity.h"
#include "mime/headers.h"

namespace imap2pst::mime {
namespace {

// ------------------------------------------------------------- encodings ---

TEST(Encodings, Base64) {
    EXPECT_EQ(decodeBase64("aGVsbG8="), "hello");
    EXPECT_EQ(decodeBase64("aGVsbG8gd29ybGQ="), "hello world");
    EXPECT_EQ(decodeBase64(""), "");
    // Line breaks and stray whitespace are what real mail looks like.
    EXPECT_EQ(decodeBase64("aGVs\r\nbG8="), "hello");
    EXPECT_EQ(decodeBase64("a G V s b G 8 ="), "hello");
    // Truncated input yields the whole bytes it does contain rather than
    // nothing: a damaged attachment should arrive damaged, not missing.
    // Six characters are 36 bits, so four whole bytes come out.
    EXPECT_EQ(decodeBase64("aGVsbG"), "hell");
    // Characters outside the alphabet are skipped, not treated as data.
    EXPECT_EQ(decodeBase64("aGV*sbG8="), "hello");
}

TEST(Encodings, QuotedPrintable) {
    EXPECT_EQ(decodeQuotedPrintable("hello"), "hello");
    EXPECT_EQ(decodeQuotedPrintable("caf=C3=A9"), "caf\xC3\xA9");
    // Soft line breaks join, with either line ending.
    EXPECT_EQ(decodeQuotedPrintable("long=\r\nline"), "longline");
    EXPECT_EQ(decodeQuotedPrintable("long=\nline"), "longline");
    // A bare '=' is kept: senders emit it, and swallowing it corrupts text.
    EXPECT_EQ(decodeQuotedPrintable("a=b"), "a=b");
    EXPECT_EQ(decodeQuotedPrintable("trailing="), "trailing");
    // "_" is a space only inside an encoded word.
    EXPECT_EQ(decodeQuotedPrintable("a_b", false), "a_b");
    EXPECT_EQ(decodeQuotedPrintable("a_b", true), "a b");
}

TEST(Encodings, BodyDecodingFollowsTheNamedEncoding) {
    EXPECT_EQ(decodeBody("aGk=", "base64"), "hi");
    EXPECT_EQ(decodeBody("aGk=", "BASE64"), "hi");
    EXPECT_EQ(decodeBody("a=20b", "quoted-printable"), "a b");
    EXPECT_EQ(decodeBody("raw", "7bit"), "raw");
    EXPECT_EQ(decodeBody("raw", "8bit"), "raw");
    // An encoding nobody recognises leaves the bytes alone, which is the only
    // choice that cannot lose data.
    EXPECT_EQ(decodeBody("raw", "x-uuencode"), "raw");
}

// --------------------------------------------------------------- charset ---

TEST(Charset, Utf8Validation) {
    EXPECT_TRUE(isValidUtf8("plain ascii"));
    EXPECT_TRUE(isValidUtf8("caf\xC3\xA9"));
    EXPECT_FALSE(isValidUtf8("\xC3"));            // truncated
    EXPECT_FALSE(isValidUtf8("\xC3\x28"));        // bad continuation
    EXPECT_FALSE(isValidUtf8("\xC0\xAF"));        // over-long
    EXPECT_FALSE(isValidUtf8("\xED\xA0\x80"));    // surrogate
}

TEST(Charset, DeclaredAsciiWithEightBitBytes) {
    // The common case in old mail: the charset says us-ascii and the bytes say
    // otherwise.  Valid UTF-8 is believed; anything else is windows-1252,
    // which maps every byte and so cannot fail.
    EXPECT_EQ(toUtf8("caf\xC3\xA9", "us-ascii"), "caf\xC3\xA9");
    EXPECT_EQ(toUtf8("caf\xE9", "us-ascii"), "caf\xC3\xA9");
}

TEST(Charset, Latin1IsDecodedAsWindows1252) {
    // 0x93 and 0x94 are undefined in ISO-8859-1 but are curly quotes in
    // windows-1252, and mail labelled the former is full of them.
    EXPECT_EQ(toUtf8("\x93hi\x94", "iso-8859-1"), "\xE2\x80\x9Chi\xE2\x80\x9D");
    EXPECT_EQ(toUtf8("caf\xE9", "iso-8859-1"), "caf\xC3\xA9");
}

TEST(Charset, RealCharsetsGoThroughIcu) {
    // KOI8-R: Cyrillic "да".
    EXPECT_EQ(toUtf8("\xC4\xC1", "koi8-r"), "\xD0\xB4\xD0\xB0");
    // Shift-JIS: "あ".
    EXPECT_EQ(toUtf8("\x82\xA0", "shift_jis"), "\xE3\x81\x82");
}

TEST(Charset, UnknownCharsetStillYieldsTheBytes) {
    // A charset nobody has heard of must not lose the message.
    EXPECT_EQ(toUtf8("hello", "x-made-up-1999"), "hello");
    EXPECT_EQ(toUtf8("caf\xE9", "x-made-up-1999"), "caf\xC3\xA9");
    EXPECT_EQ(toUtf8("caf\xC3\xA9", ""), "caf\xC3\xA9");
}

// --------------------------------------------------------------- headers ---

TEST(Headers, BlockSplittingAndUnfolding) {
    const std::string source =
        "Subject: one\r\n"
        "X-Folded: first\r\n"
        "\tsecond\r\n"
        " third\r\n"
        "To: bob@example.com\r\n"
        "\r\n"
        "the body\r\n";
    std::size_t body = 0;
    const auto fields = parseHeaderBlock(source, &body);
    ASSERT_EQ(fields.size(), 3u);
    EXPECT_EQ(fields[0].name, "Subject");
    EXPECT_EQ(fields[1].value, "first second third");
    EXPECT_EQ(source.substr(body), "the body\r\n");
}

TEST(Headers, BodyWithNoBlankLineIsStillFound) {
    // Mail with no separator exists, and losing the body to it is worse than
    // guessing where it starts.
    std::size_t body = 0;
    const std::string source = "Subject: x\r\nthis is not a header\r\n";
    const auto fields = parseHeaderBlock(source, &body);
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(source.substr(body), "this is not a header\r\n");
}

TEST(Headers, Lookup) {
    const auto fields = parseHeaderBlock("Content-Type: text/plain\r\n\r\n", nullptr);
    EXPECT_EQ(headerValue(fields, "content-type"), "text/plain");
    EXPECT_EQ(headerValue(fields, "CONTENT-TYPE"), "text/plain");
    EXPECT_TRUE(hasHeader(fields, "Content-Type"));
    EXPECT_FALSE(hasHeader(fields, "Subject"));
}

TEST(Headers, EncodedWords) {
    EXPECT_EQ(decodeEncodedWords("=?utf-8?B?w5xiZXJzaWNodA==?="), "\xC3\x9C" "bersicht");
    EXPECT_EQ(decodeEncodedWords("=?iso-8859-1?Q?caf=E9?="), "caf\xC3\xA9");
    EXPECT_EQ(decodeEncodedWords("=?utf-8?Q?a_b?="), "a b");
    // Text around an encoded word is kept as it is.
    EXPECT_EQ(decodeEncodedWords("Re: =?utf-8?B?aGk=?= (urgent)"), "Re: hi (urgent)");
    // Whitespace between two encoded words is not part of the text -- that is
    // how a long subject is split without gaining spaces.
    EXPECT_EQ(decodeEncodedWords("=?utf-8?B?aGVsbG8=?= =?utf-8?B?d29ybGQ=?="), "helloworld");
    // Anything malformed is left exactly as it arrived.
    EXPECT_EQ(decodeEncodedWords("=?utf-8?X?zzz?="), "=?utf-8?X?zzz?=");
    EXPECT_EQ(decodeEncodedWords("=?utf-8?B?unterminated"), "=?utf-8?B?unterminated");
    EXPECT_EQ(decodeEncodedWords("plain text"), "plain text");
}

TEST(Headers, StructuredFields) {
    const std::string ct = "multipart/mixed; boundary=\"a=b;c\"; charset=utf-8";
    EXPECT_EQ(fieldToken(ct), "multipart/mixed");
    // The separator inside the quoted boundary must not split the field.
    EXPECT_EQ(fieldParameter(ct, "boundary"), "a=b;c");
    EXPECT_EQ(fieldParameter(ct, "CHARSET"), "utf-8");
    EXPECT_EQ(fieldParameter(ct, "missing"), "");
    // A comment is not part of the value.
    EXPECT_EQ(fieldToken("text/plain (plain text)"), "text/plain");
}

TEST(Headers, Rfc2231Parameters) {
    // Split across continuations.
    EXPECT_EQ(fieldParameter("attachment; filename*0=\"long\"; filename*1=\"name.txt\"",
                             "filename"),
              "longname.txt");
    // With a charset and percent-encoding.
    EXPECT_EQ(fieldParameter("attachment; filename*=utf-8''caf%C3%A9.txt", "filename"),
              "caf\xC3\xA9.txt");
    // Encoded words in a parameter are not legal, but they happen.
    EXPECT_EQ(fieldParameter("attachment; filename=\"=?utf-8?B?aGk=?=.txt\"", "filename"),
              "hi.txt");
}

TEST(Headers, AddressLists) {
    auto list = parseAddressList("Alice <alice@example.com>, bob@example.net");
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].name, "Alice");
    EXPECT_EQ(list[0].email, "alice@example.com");
    EXPECT_EQ(list[1].name, "");
    EXPECT_EQ(list[1].email, "bob@example.net");

    // A comma inside a quoted display name does not split the list.
    list = parseAddressList("\"Example, Alice\" <alice@example.com>, bob@example.net");
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].name, "Example, Alice");

    // Encoded display names.
    list = parseAddressList("=?utf-8?B?w5xiZXI=?= <u@example.com>");
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].name, "\xC3\x9C" "ber");

    // Groups contribute their members.
    list = parseAddressList("Team: alice@example.com, bob@example.net;");
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].email, "alice@example.com");

    // An address with no domain keeps none: completing it would put this
    // machine's host name into someone else's mail.
    list = parseAddressList("root");
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].email, "root");

    EXPECT_TRUE(parseAddressList("").empty());
    EXPECT_TRUE(parseAddressList("   ").empty());
}


// --------------------------------------------------------------- entities ---

TEST(Entity, SimpleTextPart) {
    auto e = parseEntity("Subject: hi\r\nContent-Type: text/plain; charset=utf-8\r\n"
                         "\r\nhello\r\n");
    EXPECT_EQ(e->mediaType(), "text/plain");
    EXPECT_EQ(e->charset, "utf-8");
    EXPECT_EQ(e->body, "hello\r\n");
    EXPECT_TRUE(e->parts.empty());
}

TEST(Entity, NoContentTypeMeansTextPlain) {
    auto e = parseEntity("Subject: hi\r\n\r\nbody\r\n");
    EXPECT_EQ(e->mediaType(), "text/plain");
}

TEST(Entity, TransferEncodingIsApplied) {
    auto e = parseEntity("Content-Type: text/plain\r\n"
                         "Content-Transfer-Encoding: base64\r\n\r\naGVsbG8=\r\n");
    EXPECT_EQ(e->body, "hello");
    // The undecoded bytes are kept as well, which is what lets an attachment
    // nobody can decode still be carried across.
    EXPECT_NE(e->raw_body.find("aGVsbG8="), std::string::npos);
}

TEST(Entity, MultipartIsSplitOnItsBoundary) {
    const std::string source =
        "Content-Type: multipart/mixed; boundary=\"sep\"\r\n"
        "\r\n"
        "preamble text, which is not a part\r\n"
        "--sep\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "first\r\n"
        "--sep\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<p>second</p>\r\n"
        "--sep--\r\n"
        "epilogue, also not a part\r\n";
    auto e = parseEntity(source);
    ASSERT_EQ(e->parts.size(), 2u);
    EXPECT_EQ(e->parts[0]->mediaType(), "text/plain");
    EXPECT_EQ(e->parts[0]->body, "first");
    EXPECT_EQ(e->parts[1]->mediaType(), "text/html");
    EXPECT_EQ(e->parts[1]->body, "<p>second</p>");
}

TEST(Entity, BoundaryMustStartItsOwnLine) {
    // A body that mentions the boundary mid-line must not be split there.
    const std::string source =
        "Content-Type: multipart/mixed; boundary=sep\r\n\r\n"
        "--sep\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "the text --sep is quoted here\r\n"
        "--sep--\r\n";
    auto e = parseEntity(source);
    ASSERT_EQ(e->parts.size(), 1u);
    EXPECT_EQ(e->parts[0]->body, "the text --sep is quoted here");
}

TEST(Entity, UnterminatedMultipartKeepsWhatItHas) {
    // Truncated mail is common in archives; the parts before the truncation
    // should still arrive.
    const std::string source =
        "Content-Type: multipart/mixed; boundary=sep\r\n\r\n"
        "--sep\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "survives\r\n";
    auto e = parseEntity(source);
    ASSERT_EQ(e->parts.size(), 1u);
    EXPECT_EQ(e->parts[0]->body, "survives\r\n");
}

TEST(Entity, MultipartWithoutBoundaryKeepsItsBody) {
    // Nothing can be split, but the content must not be dropped.
    auto e = parseEntity("Content-Type: multipart/mixed\r\n\r\nlost structure\r\n");
    EXPECT_TRUE(e->parts.empty());
    EXPECT_NE(e->raw_body.find("lost structure"), std::string::npos);
}

TEST(Entity, NestedMultipart) {
    const std::string source =
        "Content-Type: multipart/mixed; boundary=outer\r\n\r\n"
        "--outer\r\n"
        "Content-Type: multipart/alternative; boundary=inner\r\n\r\n"
        "--inner\r\n"
        "Content-Type: text/plain\r\n\r\nplain\r\n"
        "--inner\r\n"
        "Content-Type: text/html\r\n\r\n<p>rich</p>\r\n"
        "--inner--\r\n"
        "--outer\r\n"
        "Content-Type: application/pdf\r\n"
        "Content-Disposition: attachment; filename=\"report.pdf\"\r\n\r\n"
        "%PDF-1.4\r\n"
        "--outer--\r\n";
    auto e = parseEntity(source);
    ASSERT_EQ(e->parts.size(), 2u);
    ASSERT_EQ(e->parts[0]->parts.size(), 2u);
    EXPECT_EQ(e->parts[0]->parts[1]->mediaType(), "text/html");
    EXPECT_EQ(e->parts[1]->filename, "report.pdf");
    EXPECT_EQ(e->parts[1]->disposition, "attachment");
}

TEST(Entity, EmbeddedMessageIsParsedAsOne) {
    const std::string source =
        "Content-Type: message/rfc822\r\n\r\n"
        "From: carol@example.org\r\n"
        "Subject: inner\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "inner body\r\n";
    auto e = parseEntity(source);
    EXPECT_TRUE(e->isMessage());
    ASSERT_EQ(e->parts.size(), 1u);
    EXPECT_EQ(headerValue(e->parts[0]->headers, "Subject"), "inner");
    EXPECT_EQ(e->parts[0]->body, "inner body\r\n");
}

TEST(Entity, NestingIsBounded) {
    // A message that nests without end must not take the stack with it.
    std::string source;
    for (int i = 0; i < 200; ++i) {
        source += "Content-Type: message/rfc822\r\n\r\n";
    }
    source += "Content-Type: text/plain\r\n\r\nbottom\r\n";
    auto e = parseEntity(source);
    int depth = 0;
    for (const Entity* p = e.get(); !p->parts.empty(); p = p->parts[0].get()) ++depth;
    EXPECT_LE(depth, kMaxNestingDepth);
}

TEST(Entity, FilenameFromEitherHeader) {
    auto disp = parseEntity("Content-Type: application/octet-stream\r\n"
                            "Content-Disposition: attachment; filename=\"a.bin\"\r\n\r\nx");
    EXPECT_EQ(disp->filename, "a.bin");
    // Older senders put it on Content-Type instead.
    auto ct = parseEntity("Content-Type: application/octet-stream; name=\"b.bin\"\r\n\r\nx");
    EXPECT_EQ(ct->filename, "b.bin");
}

TEST(Entity, ContentIdLosesItsAngleBrackets) {
    auto e = parseEntity("Content-Type: image/png\r\n"
                         "Content-ID: <image001@example.com>\r\n\r\nx");
    EXPECT_EQ(e->content_id, "image001@example.com");
}

}  // namespace
}  // namespace imap2pst::mime

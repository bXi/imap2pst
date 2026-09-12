// IMAP response parsing, driven entirely by recorded fixtures: no server and no
// network are involved.

#include <gtest/gtest.h>

#include "imap/imap_parse.h"
#include "test_util.h"

namespace imap2pst::imap {
namespace {

TEST(ImapList, ParsesHierarchyAndDelimiters) {
    const auto folders = parseListResponse(test::readFixture("list_response.txt"));
    ASSERT_EQ(folders.size(), 7u);

    EXPECT_EQ(folders[0].full_name, "INBOX");
    EXPECT_EQ(folders[0].delimiter, '.');
    EXPECT_TRUE(folders[0].selectable);

    EXPECT_EQ(folders[2].full_name, "INBOX.Work.2024");
    EXPECT_EQ(folders[2].leaf_name(), "2024");
    const auto path = folders[2].path();
    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[0], "INBOX");
    EXPECT_EQ(path[1], "Work");
    EXPECT_EQ(path[2], "2024");
}

TEST(ImapList, MarksNoselectFolders) {
    const auto folders = parseListResponse(test::readFixture("list_response.txt"));
    ASSERT_GE(folders.size(), 4u);
    EXPECT_EQ(folders[3].full_name, "Archive");
    EXPECT_FALSE(folders[3].selectable);
    EXPECT_TRUE(folders[4].selectable);
}

TEST(ImapList, HandlesNilDelimiterAndEscapedQuotes) {
    const auto folders = parseListResponse(test::readFixture("list_response.txt"));
    ASSERT_EQ(folders.size(), 7u);

    // A NIL delimiter means the mailbox name has no hierarchy at all.
    EXPECT_EQ(folders[5].full_name, "Flat Folder");
    EXPECT_EQ(folders[5].delimiter, '\0');
    const auto flat = folders[5].path();
    ASSERT_EQ(flat.size(), 1u);
    EXPECT_EQ(flat[0], "Flat Folder");

    EXPECT_EQ(folders[6].full_name, "Quoted \"Name\" Folder");
}

TEST(ImapList, DecodesModifiedUtf7MailboxNames) {
    // RFC 3501 5.1.3.  Servers send non-ASCII mailbox names this way, so a
    // folder called "Übersicht" arrives as "&ANw-bersicht".
    EXPECT_EQ(decodeModifiedUtf7("&ANw-bersicht"), "\xC3\x9C" "bersicht");
    EXPECT_EQ(decodeModifiedUtf7("INBOX"), "INBOX");
    EXPECT_EQ(decodeModifiedUtf7(" Important"), " Important");
    // "&-" is a literal ampersand.
    EXPECT_EQ(decodeModifiedUtf7("Rock &- Roll"), "Rock & Roll");
    // Japanese: "日本語"
    EXPECT_EQ(decodeModifiedUtf7("&ZeVnLIqe-"), "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
    // Outside the BMP, which needs a surrogate pair: U+1F600
    EXPECT_EQ(decodeModifiedUtf7("&2D3eAA-"), "\xF0\x9F\x98\x80");
    // A malformed run is kept rather than silently dropped.
    EXPECT_EQ(decodeModifiedUtf7("&!!!-x"), "&!!!-x");
}

TEST(ImapList, KeepsTheRawNameForTheServer) {
    const auto folders = parseListResponse(
        "* LIST (\\HasNoChildren) \".\" &ANw-bersicht\r\n");
    ASSERT_EQ(folders.size(), 1u);
    EXPECT_EQ(folders[0].raw_name, "&ANw-bersicht");
    EXPECT_EQ(folders[0].full_name, "\xC3\x9C" "bersicht");
    EXPECT_EQ(folders[0].leaf_name(), "\xC3\x9C" "bersicht");
}

TEST(ImapFetch, ParsesUidFlagsAndInternalDate) {
    const auto metas = parseFetchResponse(test::readFixture("fetch_response.txt"));
    ASSERT_EQ(metas.size(), 4u);

    EXPECT_EQ(metas[0].uid, 101u);
    EXPECT_EQ(metas[0].flags, kFlagSeen);
    EXPECT_EQ(metas[0].size, 1234u);
    // 17-Jul-1996 02:44:25 -0700 == 837596665 UTC
    EXPECT_EQ(metas[0].internal_date, 837596665);

    EXPECT_EQ(metas[1].uid, 102u);
    EXPECT_EQ(metas[1].flags, kFlagSeen | kFlagAnswered);
    ASSERT_EQ(metas[1].keywords.size(), 1u);
    EXPECT_EQ(metas[1].keywords[0], "$Label1");
    EXPECT_EQ(metas[1].internal_date, 1700000000);

    EXPECT_EQ(metas[2].uid, 103u);
    EXPECT_EQ(metas[2].flags, kFlagNone);

    EXPECT_EQ(metas[3].flags, kFlagFlagged | kFlagDeleted | kFlagDraft);
}

TEST(ImapFetch, SingleDigitDayAndPositiveZone) {
    // " 2-Jan-2020 05:06:07 +0530" -> 2020-01-01T23:36:07Z
    EXPECT_EQ(parseInternalDate(" 2-Jan-2020 05:06:07 +0530"), 1577921767);
}

TEST(ImapFetch, RejectsGarbageInternalDate) {
    EXPECT_EQ(parseInternalDate("not a date"), 0);
    EXPECT_EQ(parseInternalDate("17-Xxx-1996 02:44:25 -0700"), 0);
}

TEST(ImapFlags, MapsSystemFlagsAndKeepsKeywords) {
    std::vector<std::string> keywords;
    const std::uint32_t flags = parseFlags("(\\Seen \\Flagged Important $Junk)", &keywords);
    EXPECT_EQ(flags, kFlagSeen | kFlagFlagged);
    ASSERT_EQ(keywords.size(), 2u);
    EXPECT_EQ(keywords[0], "Important");
    EXPECT_EQ(keywords[1], "$Junk");
}

}  // namespace
}  // namespace imap2pst::imap

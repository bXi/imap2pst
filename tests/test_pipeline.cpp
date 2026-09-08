// End-to-end: recorded IMAP responses -> vmime -> PST, with the resulting file
// re-read by the independent test parser.  Still no network and no server.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>

#include "imap/imap_client.h"
#include "pipeline/pipeline.h"
#include "pst_reader.h"
#include "test_util.h"

namespace imap2pst {
namespace {

using pst::test::readAll;
using pst::test::Reader;
using pst::test::TempPst;

// Serves a small two-folder account built out of the .eml fixtures.
class FixtureTransport : public imap::ImapTransport {
 public:
    FixtureTransport() {
        folders_ =
            "* LIST (\\HasChildren) \".\" \"INBOX\"\r\n"
            "* LIST (\\HasNoChildren) \".\" \"INBOX.Work\"\r\n"
            "* LIST (\\Noselect \\HasChildren) \".\" \"Archive\"\r\n";

        add("INBOX", 1, "plain.eml", kFlagSeen, 1700000000);
        add("INBOX", 2, "multipart_mixed.eml", kFlagNone, 1700000100);
        add("INBOX", 3, "alternative_latin1.eml", kFlagSeen | kFlagFlagged, 1700000200);
        add("INBOX.Work", 4, "html_only.eml", kFlagNone, 1700000300);
        add("INBOX.Work", 5, "utf8_body.eml", kFlagSeen, 1700000400);
    }

    std::string command(const std::string& mailbox,
                        const std::string& command_line) override {
        if (command_line.rfind("LIST", 0) == 0) return folders_;
        std::string response;
        for (const auto& m : messages_) {
            if (m.mailbox != mailbox) continue;
            response += "* 1 FETCH (UID " + std::to_string(m.uid) + " FLAGS (" +
                        m.flag_text + ") INTERNALDATE \"" + m.internal_date +
                        "\" RFC822.SIZE " + std::to_string(m.raw.size()) + ")\r\n";
        }
        return response;
    }

    std::string fetchBody(const std::string& mailbox, std::uint32_t uid) override {
        for (const auto& m : messages_) {
            if (m.mailbox == mailbox && m.uid == uid) return m.raw;
        }
        throw imap::ImapError("no such message");
    }

 private:
    struct Entry {
        std::string mailbox;
        std::uint32_t uid;
        std::string raw;
        std::string flag_text;
        std::string internal_date;
    };

    void add(const std::string& mailbox, std::uint32_t uid, const std::string& fixture,
             std::uint32_t flags, std::int64_t /*unused*/) {
        std::string flag_text;
        if (flags & kFlagSeen) flag_text += "\\Seen ";
        if (flags & kFlagFlagged) flag_text += "\\Flagged";
        messages_.push_back({mailbox, uid, test::readFixture(fixture), flag_text,
                             "14-Nov-2023 22:13:20 +0000"});
    }

    std::string folders_;
    std::vector<Entry> messages_;
};

TEST(Pipeline, MigratesFoldersAndMessagesIntoAValidPst) {
    TempPst tmp;
    auto transport = std::make_shared<FixtureTransport>();
    imap::ImapClient client(transport);

    PipelineOptions options;
    options.output_path = tmp.path();

    const PipelineStats stats = run(client, options);

    // Three folders listed, five messages, three attachments (two in the
    // multipart fixture, none elsewhere).
    EXPECT_EQ(stats.folders, 3u);
    EXPECT_EQ(stats.messages, 5u);
    EXPECT_EQ(stats.attachments, 2u);
    EXPECT_EQ(stats.failed_messages, 0u);

    const Reader r(readAll(tmp.path()));
    EXPECT_EQ(r.ibFileEof(), r.size());

    const auto nids = r.nodes();
    const std::set<pst::Nid> present(nids.begin(), nids.end());
    EXPECT_TRUE(present.count(pst::kNidMessageStore));
    EXPECT_TRUE(present.count(pst::kNidNameToIdMap));
    EXPECT_TRUE(present.count(pst::kNidRootFolder));

    std::size_t message_nodes = 0, folder_nodes = 0;
    for (pst::Nid nid : nids) {
        if (pst::nidType(nid) == pst::kNidTypeNormalMessage) ++message_nodes;
        if (pst::nidType(nid) == pst::kNidTypeNormalFolder) ++folder_nodes;
    }
    EXPECT_EQ(message_nodes, 5u);
    // Root, "Top of Personal Folders", INBOX, INBOX/Work and Archive.
    EXPECT_EQ(folder_nodes, 5u);
}

TEST(Pipeline, FolderFilterRestrictsWhatIsMigrated) {
    TempPst tmp;
    auto transport = std::make_shared<FixtureTransport>();
    imap::ImapClient client(transport);

    PipelineOptions options;
    options.output_path = tmp.path();
    options.folders = {"INBOX.Work"};

    const PipelineStats stats = run(client, options);
    EXPECT_EQ(stats.folders, 1u);
    EXPECT_EQ(stats.messages, 2u);

    const Reader r(readAll(tmp.path()));
    std::size_t message_nodes = 0;
    for (pst::Nid nid : r.nodes()) {
        if (pst::nidType(nid) == pst::kNidTypeNormalMessage) ++message_nodes;
    }
    EXPECT_EQ(message_nodes, 2u);
}

TEST(Pipeline, UnknownFolderIsReportedNotFatal) {
    TempPst tmp;
    auto transport = std::make_shared<FixtureTransport>();
    imap::ImapClient client(transport);

    PipelineOptions options;
    options.output_path = tmp.path();
    options.folders = {"Does.Not.Exist"};

    std::vector<std::string> log;
    const PipelineStats stats =
        run(client, options, [&](const std::string& m) { log.push_back(m); });

    EXPECT_EQ(stats.folders, 0u);
    EXPECT_EQ(stats.messages, 0u);
    const bool warned = std::any_of(log.begin(), log.end(), [](const std::string& m) {
        return m.find("folder not found") != std::string::npos;
    });
    EXPECT_TRUE(warned) << "the missing folder should have been reported";
}

}  // namespace
}  // namespace imap2pst

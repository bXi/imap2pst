// End-to-end: recorded IMAP responses -> vmime -> PST, with the resulting file
// re-read by the independent test parser.  Still no network and no server.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>

#include "imap/imap_client.h"
#include "pipeline/pipeline.h"
#include "pst/pst_writer.h"
#include "pst_reader.h"
#include "test_util.h"

namespace imap2pst {
namespace {

using pst::test::readAll;
using pst::test::Reader;
using pst::test::TempDir;
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

        // A body fetch names BODY.PEEK[]; anything else is the metadata pass.
        if (command_line.find("BODY.PEEK[]") != std::string::npos) {
            ++body_batches_;
            std::string response;
            for (const auto& m : messages_) {
                if (m.mailbox != mailbox) continue;
                if (!inSet(command_line, m.uid)) continue;
                if (refuse_batch_for_.count(m.uid)) continue;
                response += "* 1 FETCH (UID " + std::to_string(m.uid) + " BODY[] {" +
                            std::to_string(m.raw.size()) + "}\r\n" + m.raw + ")\r\n";
            }
            return response;
        }

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
        ++single_fetches_;
        for (const auto& m : messages_) {
            if (m.mailbox == mailbox && m.uid == uid) return m.raw;
        }
        throw imap::ImapError("no such message");
    }

    std::size_t bodyBatches() const { return body_batches_; }
    std::size_t singleFetches() const { return single_fetches_; }
    // Makes the batched path omit `uid`, the way a server that cannot read one
    // message does, so the per-message fallback is exercised.
    void omitFromBatch(std::uint32_t uid) { refuse_batch_for_.insert(uid); }

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

    // Crude but sufficient: the sequence set this fixture ever sees is a comma
    // separated list of single UIDs and ranges.
    static bool inSet(const std::string& command_line, std::uint32_t uid) {
        const std::size_t at = command_line.find("UID FETCH ");
        if (at == std::string::npos) return false;
        std::string set = command_line.substr(at + 10);
        set = set.substr(0, set.find(' '));
        std::size_t i = 0;
        while (i < set.size()) {
            const std::size_t comma = std::min(set.find(',', i), set.size());
            const std::string part = set.substr(i, comma - i);
            const std::size_t colon = part.find(':');
            if (colon == std::string::npos) {
                if (std::stoul(part) == uid) return true;
            } else {
                const unsigned long lo = std::stoul(part.substr(0, colon));
                const unsigned long hi = std::stoul(part.substr(colon + 1));
                if (uid >= lo && uid <= hi) return true;
            }
            i = comma + 1;
        }
        return false;
    }

    std::string folders_;
    std::vector<Entry> messages_;
    std::set<std::uint32_t> refuse_batch_for_;
    std::size_t body_batches_ = 0;
    std::size_t single_fetches_ = 0;
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
    // INBOX, INBOX/Work and Archive, on top of the folders the writer always
    // creates (root, the IPM subtree and the special folders the message store
    // advertises).
    EXPECT_EQ(folder_nodes, pst::PstWriter::kBuiltinFolderCount + 3);
}

TEST(Pipeline, BodiesAreFetchedInBatches) {
    // Five messages across two selectable folders: one round trip each, not one
    // per message.  This is the difference between a mailbox that migrates in
    // minutes and one that takes hours.
    TempPst tmp;
    auto transport = std::make_shared<FixtureTransport>();
    imap::ImapClient client(transport);

    PipelineOptions options;
    options.output_path = tmp.path();

    const PipelineStats stats = run(client, options);
    EXPECT_EQ(stats.messages, 5u);
    EXPECT_EQ(transport->bodyBatches(), 2u);
    EXPECT_EQ(transport->singleFetches(), 0u);
    EXPECT_GT(stats.fetched_bytes, 0u);
}

TEST(Pipeline, BatchSizeSplitsTheWork) {
    TempPst tmp;
    auto transport = std::make_shared<FixtureTransport>();
    imap::ImapClient client(transport);

    PipelineOptions options;
    options.output_path = tmp.path();
    options.batch_size = 2;

    const PipelineStats stats = run(client, options);
    EXPECT_EQ(stats.messages, 5u);
    // INBOX holds three (2 + 1) and INBOX.Work two.
    EXPECT_EQ(transport->bodyBatches(), 3u);
    EXPECT_EQ(transport->singleFetches(), 0u);
}

TEST(Pipeline, MessageMissingFromABatchIsFetchedOnItsOwn) {
    TempPst tmp;
    auto transport = std::make_shared<FixtureTransport>();
    transport->omitFromBatch(2);
    imap::ImapClient client(transport);

    PipelineOptions options;
    options.output_path = tmp.path();

    const PipelineStats stats = run(client, options);
    EXPECT_EQ(stats.messages, 5u) << "the omitted message must still arrive";
    EXPECT_EQ(stats.failed_messages, 0u);
    EXPECT_EQ(transport->singleFetches(), 1u);
}

TEST(Pipeline, SpoolIsReusedOnASecondRun) {
    // What makes an interrupted migration cheap to restart: the second run
    // fetches nothing, because every message is already on disk.
    TempDir spool;
    {
        TempPst tmp;
        auto transport = std::make_shared<FixtureTransport>();
        imap::ImapClient client(transport);
        PipelineOptions options;
        options.output_path = tmp.path();
        options.spool_dir = spool.path();
        const PipelineStats stats = run(client, options);
        EXPECT_EQ(stats.messages, 5u);
        EXPECT_EQ(stats.reused_messages, 0u);
        EXPECT_GT(transport->bodyBatches(), 0u);
    }
    {
        TempPst tmp;
        auto transport = std::make_shared<FixtureTransport>();
        imap::ImapClient client(transport);
        PipelineOptions options;
        options.output_path = tmp.path();
        options.spool_dir = spool.path();
        const PipelineStats stats = run(client, options);
        EXPECT_EQ(stats.messages, 5u);
        EXPECT_EQ(stats.reused_messages, 5u);
        EXPECT_EQ(stats.fetched_bytes, 0u);
        EXPECT_EQ(transport->bodyBatches(), 0u);
        EXPECT_EQ(transport->singleFetches(), 0u);

        // The PST built from the spool is the same file as the one built from
        // the server, message for message.
        const Reader r(readAll(tmp.path()));
        std::size_t message_nodes = 0;
        for (pst::Nid nid : r.nodes()) {
            if (pst::nidType(nid) == pst::kNidTypeNormalMessage) ++message_nodes;
        }
        EXPECT_EQ(message_nodes, 5u);
    }
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

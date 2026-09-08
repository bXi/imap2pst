// ImapClient driven by a fake transport that replays recorded responses.  No
// network, no server, no libcurl involved.

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>

#include "imap/imap_client.h"
#include "test_util.h"

namespace imap2pst::imap {
namespace {

// Replays fixture responses and records what was asked for, so the tests can
// assert on the commands the client issues as well as on what it returns.
class FakeTransport : public ImapTransport {
 public:
    std::string command(const std::string& mailbox,
                        const std::string& command_line) override {
        commands.push_back({mailbox, command_line});
        if (command_line.rfind("LIST", 0) == 0) {
            return test::readFixture("list_response.txt");
        }
        if (command_line.rfind("UID FETCH", 0) == 0) {
            auto it = fetch_by_mailbox.find(mailbox);
            if (it != fetch_by_mailbox.end()) return it->second;
            return test::readFixture("fetch_response.txt");
        }
        return {};
    }

    std::string fetchBody(const std::string& mailbox, std::uint32_t uid) override {
        bodies_requested.push_back({mailbox, uid});
        if (fail_uid && uid == fail_uid) throw ImapError("simulated fetch failure");
        return "Subject: Message " + std::to_string(uid) +
               "\r\nFrom: s@example.com\r\n\r\nBody of " + std::to_string(uid) + "\r\n";
    }

    std::vector<std::pair<std::string, std::string>> commands;
    std::vector<std::pair<std::string, std::uint32_t>> bodies_requested;
    std::map<std::string, std::string> fetch_by_mailbox;
    std::uint32_t fail_uid = 0;
};

TEST(ImapClient, ListFoldersIssuesListAndParsesIt) {
    auto fake = std::make_shared<FakeTransport>();
    ImapClient client(fake);

    const auto folders = client.listFolders();
    ASSERT_EQ(folders.size(), 7u);
    EXPECT_EQ(folders[0].full_name, "INBOX");

    ASSERT_EQ(fake->commands.size(), 1u);
    EXPECT_EQ(fake->commands[0].first, "");
    EXPECT_EQ(fake->commands[0].second, "LIST \"\" \"*\"");
}

TEST(ImapClient, ListMessagesSelectsTheMailbox) {
    auto fake = std::make_shared<FakeTransport>();
    ImapClient client(fake);

    const auto metas = client.listMessages("INBOX.Work");
    ASSERT_EQ(metas.size(), 4u);
    EXPECT_EQ(metas[0].uid, 101u);

    ASSERT_EQ(fake->commands.size(), 1u);
    EXPECT_EQ(fake->commands[0].first, "INBOX.Work");
    EXPECT_EQ(fake->commands[0].second,
              "UID FETCH 1:* (FLAGS INTERNALDATE RFC822.SIZE)");
}

TEST(ImapClient, FetchFolderPairsBodiesWithTheirMetadata) {
    auto fake = std::make_shared<FakeTransport>();
    ImapClient client(fake);

    const auto raws = client.fetchFolder("INBOX");
    ASSERT_EQ(raws.size(), 4u);

    EXPECT_EQ(raws[0].uid, 101u);
    EXPECT_EQ(raws[0].flags, kFlagSeen);
    EXPECT_EQ(raws[0].internal_date, 837596665);
    EXPECT_NE(raws[0].rfc822.find("Body of 101"), std::string::npos);

    EXPECT_EQ(raws[3].flags, kFlagFlagged | kFlagDeleted | kFlagDraft);

    // One body request per message, all against the selected mailbox.
    ASSERT_EQ(fake->bodies_requested.size(), 4u);
    for (const auto& request : fake->bodies_requested) {
        EXPECT_EQ(request.first, "INBOX");
    }
}

TEST(ImapClient, EmptyMailboxYieldsNoMessages) {
    auto fake = std::make_shared<FakeTransport>();
    fake->fetch_by_mailbox["Empty"] = "";
    ImapClient client(fake);

    EXPECT_TRUE(client.fetchFolder("Empty").empty());
    EXPECT_TRUE(fake->bodies_requested.empty());
}

TEST(ImapClient, TransportFailurePropagates) {
    auto fake = std::make_shared<FakeTransport>();
    fake->fail_uid = 102;
    ImapClient client(fake);
    EXPECT_THROW(client.fetchFolder("INBOX"), ImapError);
}

}  // namespace
}  // namespace imap2pst::imap

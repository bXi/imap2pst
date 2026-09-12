#pragma once

// IMAP fetch module.
//
// Transport is behind an interface so the parsing and folder-walking logic can
// be exercised against recorded responses.  CurlTransport is the real one; it
// uses libcurl's native imap:// / imaps:// support.

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/message.h"
#include "imap/imap_parse.h"

namespace imap2pst::imap {

class ImapError : public std::runtime_error {
 public:
    using std::runtime_error::runtime_error;
};

struct ImapConfig {
    std::string host;
    int port = 0;             // 0 -> 993 for imaps, 143 otherwise
    bool use_tls = true;
    bool verify_peer = true;
    std::string username;
    std::string password;
    std::string oauth2_bearer;  // used instead of `password` when set
    long timeout_seconds = 120;
    bool verbose = false;
    // A dropped connection or a server hiccup part way through a large mailbox
    // should not cost the whole run, so every request is retried.
    int retry_attempts = 3;
    long retry_backoff_ms = 500;
};

// Issues commands against a server.  One instance is bound to one account.
class ImapTransport {
 public:
    virtual ~ImapTransport() = default;

    // Runs `command` with `mailbox` selected (empty for none) and returns the
    // untagged response text.
    virtual std::string command(const std::string& mailbox,
                                const std::string& command_line) = 0;

    // Returns the raw RFC 822 octets of one message.
    virtual std::string fetchBody(const std::string& mailbox, std::uint32_t uid) = 0;
};

class CurlTransport : public ImapTransport {
 public:
    explicit CurlTransport(ImapConfig config);
    ~CurlTransport() override;

    std::string command(const std::string& mailbox,
                        const std::string& command_line) override;
    std::string fetchBody(const std::string& mailbox, std::uint32_t uid) override;

 private:
    std::string url(const std::string& mailbox, const std::string& suffix = {}) const;
    std::string run(const std::string& url, const std::string& custom_request);
    std::string withRetries(const std::function<std::string()>& fn);

    ImapConfig config_;
    void* curl_ = nullptr;  // CURL*
};

class ImapClient {
 public:
    explicit ImapClient(std::shared_ptr<ImapTransport> transport)
        : transport_(std::move(transport)) {}

    // LIST "" "*" -- every folder, with hierarchy delimiters preserved.
    std::vector<FolderInfo> listFolders();

    // Metadata for every message in `mailbox`, without bodies.
    std::vector<MessageMeta> listMessages(const std::string& mailbox);

    // Metadata plus the full RFC 822 source for every message in `mailbox`.
    std::vector<RawMessage> fetchFolder(const std::string& mailbox);

    RawMessage fetchMessage(const std::string& mailbox, const MessageMeta& meta);

    // Fetches the bodies for `metas` in one round trip instead of one per
    // message.  The result is aligned with `metas`; an entry whose `rfc822` is
    // empty was not returned by the server and should be retried on its own so
    // that a single bad message cannot fail a whole batch.
    std::vector<RawMessage> fetchMessages(const std::string& mailbox,
                                          const std::vector<MessageMeta>& metas);

 private:
    std::shared_ptr<ImapTransport> transport_;
};

}  // namespace imap2pst::imap

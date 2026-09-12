#pragma once

// Glue: IMAP -> MIME -> PST.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "imap/imap_client.h"

namespace imap2pst {

struct PipelineOptions {
    std::string output_path;
    // Folders to migrate, by their server-side full name.  Empty means "every
    // selectable folder the server lists".
    std::vector<std::string> folders;
    bool verbose = false;

    // How many messages to ask for per round trip.  One UID FETCH naming many
    // messages is far cheaper than one per message, but the whole batch is held
    // in memory, so `batch_bytes` caps a batch that turns out to be large.
    std::size_t batch_size = 50;
    std::size_t batch_bytes = 32u * 1024 * 1024;

    // Directory holding fetched message source.  When set, a message already in
    // the spool is not fetched again, so a re-run after an interruption pays
    // only for what it had not yet downloaded.  The PST itself is always
    // written from scratch.
    std::string spool_dir;

    // How often to report progress, in messages.  0 turns it off.
    std::size_t progress_every = 100;
};

// A message that could not be migrated, kept so the run can name it rather
// than only count it.
struct FailedMessage {
    std::string folder;
    std::uint32_t uid = 0;
    std::string reason;
};

struct PipelineStats {
    std::size_t folders = 0;
    std::size_t messages = 0;
    std::size_t attachments = 0;
    std::size_t failed_messages = 0;
    // Message source bytes fetched from the server, and the count that came
    // from the spool instead.
    std::uint64_t fetched_bytes = 0;
    std::size_t reused_messages = 0;
    std::vector<FailedMessage> failed_uids;
};

// Runs the migration and writes the PST.  Progress is reported through `log`
// when it is set.
PipelineStats run(imap::ImapClient& client, const PipelineOptions& options,
                  const std::function<void(const std::string&)>& log = {});

}  // namespace imap2pst

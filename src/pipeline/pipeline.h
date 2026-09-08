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
};

struct PipelineStats {
    std::size_t folders = 0;
    std::size_t messages = 0;
    std::size_t attachments = 0;
    std::size_t failed_messages = 0;
};

// Runs the migration and writes the PST.  Progress is reported through `log`
// when it is set.
PipelineStats run(imap::ImapClient& client, const PipelineOptions& options,
                  const std::function<void(const std::string&)>& log = {});

}  // namespace imap2pst

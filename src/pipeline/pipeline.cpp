#include "pipeline/pipeline.h"

#include <algorithm>
#include <map>

#include "mime/mime_parser.h"
#include "pst/pst_writer.h"

namespace imap2pst {
namespace {

void report(const std::function<void(const std::string&)>& log, const std::string& msg) {
    if (log) log(msg);
}

}  // namespace

PipelineStats run(imap::ImapClient& client, const PipelineOptions& options,
                  const std::function<void(const std::string&)>& log) {
    PipelineStats stats;

    report(log, "Listing folders");
    std::vector<FolderInfo> folders = client.listFolders();

    if (!options.folders.empty()) {
        std::vector<FolderInfo> filtered;
        for (const auto& f : folders) {
            if (std::find(options.folders.begin(), options.folders.end(), f.full_name) !=
                options.folders.end()) {
                filtered.push_back(f);
            }
        }
        for (const auto& want : options.folders) {
            const bool found = std::any_of(filtered.begin(), filtered.end(),
                                           [&](const FolderInfo& f) { return f.full_name == want; });
            if (!found) report(log, "warning: folder not found on server: " + want);
        }
        folders = std::move(filtered);
    }

    // Shortest paths first, so a parent always exists before its children.
    std::sort(folders.begin(), folders.end(), [](const FolderInfo& a, const FolderInfo& b) {
        const auto pa = a.path();
        const auto pb = b.path();
        if (pa.size() != pb.size()) return pa.size() < pb.size();
        return a.full_name < b.full_name;
    });

    pst::PstWriter writer(options.output_path);

    // Maps a folder path prefix ("INBOX/Work") to the PST folder holding it, so
    // intermediate folders the server did not list still get created.
    std::map<std::string, pst::FolderId> by_path;

    auto folder_for = [&](const std::vector<std::string>& path) {
        pst::FolderId parent = writer.ipmSubtree();
        std::string prefix;
        for (const auto& part : path) {
            if (!prefix.empty()) prefix += "\x1f";
            prefix += part;
            auto it = by_path.find(prefix);
            if (it == by_path.end()) {
                const pst::FolderId id = writer.createFolder(parent, part);
                it = by_path.emplace(prefix, id).first;
            }
            parent = it->second;
        }
        return parent;
    };

    for (const auto& folder : folders) {
        const auto path = folder.path();
        if (path.empty()) continue;
        const pst::FolderId id = folder_for(path);
        ++stats.folders;

        if (!folder.selectable) {
            report(log, "Folder " + folder.full_name + " is \\Noselect; created empty");
            continue;
        }

        report(log, "Fetching " + folder.full_name);
        std::vector<imap::MessageMeta> metas;
        try {
            metas = client.listMessages(folder.full_name);
        } catch (const imap::ImapError& e) {
            report(log, "warning: " + folder.full_name + ": " + e.what());
            continue;
        }

        for (const auto& meta : metas) {
            RawMessage raw;
            try {
                raw = client.fetchMessage(folder.full_name, meta);
            } catch (const imap::ImapError& e) {
                ++stats.failed_messages;
                report(log, "warning: UID " + std::to_string(meta.uid) + ": " + e.what());
                continue;
            }
            Message msg = mime::parse(raw);
            if (meta.size) msg.size = meta.size;
            writer.addMessage(id, msg);
            ++stats.messages;
            stats.attachments += msg.attachments.size();
        }
        report(log, "  " + folder.full_name + ": " + std::to_string(metas.size()) +
                        " message(s)");
    }

    writer.finish();
    return stats;
}

}  // namespace imap2pst

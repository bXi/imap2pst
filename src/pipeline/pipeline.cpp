#include "pipeline/pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "mime/mime_parser.h"
#include "pst/pst_writer.h"

namespace imap2pst {
namespace {

void report(const std::function<void(const std::string&)>& log, const std::string& msg) {
    if (log) log(msg);
}

// A spool path is derived from the folder and UID rather than the message id,
// because the pair is what the server guarantees to be stable.
std::filesystem::path spoolPath(const std::string& dir, const std::string& folder,
                                std::uint32_t uid) {
    // Folder names carry delimiters and non-ASCII bytes; hash them into a name
    // the filesystem is happy with instead of trying to sanitise them.
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : folder) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "%016llx-%010u.eml",
                  static_cast<unsigned long long>(h), uid);
    return std::filesystem::path(dir) / name;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeFile(const std::filesystem::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

std::string humanBytes(std::uint64_t n) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; ++u; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), v < 10.0 ? "%.1f %s" : "%.0f %s", v, kUnits[u]);
    return buf;
}

}  // namespace

PipelineStats run(imap::ImapClient& client, const PipelineOptions& options,
                  const std::function<void(const std::string&)>& log) {
    PipelineStats stats;
    const auto started = std::chrono::steady_clock::now();

    if (!options.spool_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(options.spool_dir, ec);
        if (ec) report(log, "warning: cannot create spool directory: " + ec.message());
    }

    report(log, "Listing folders");
    std::vector<FolderInfo> folders = client.listFolders();

    if (!options.folders.empty()) {
        std::vector<FolderInfo> filtered;
        for (const auto& f : folders) {
            // Accept either the display name or the raw one, so --folder works
            // whichever the user copied.
            if (std::find(options.folders.begin(), options.folders.end(), f.full_name) !=
                    options.folders.end() ||
                std::find(options.folders.begin(), options.folders.end(), f.raw_name) !=
                    options.folders.end()) {
                filtered.push_back(f);
            }
        }
        for (const auto& want : options.folders) {
            const bool found = std::any_of(
                filtered.begin(), filtered.end(), [&](const FolderInfo& f) {
                    return f.full_name == want || f.raw_name == want;
                });
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

    // Only known once a folder has been listed; used to give progress a
    // denominator that grows as the walk goes on.
    std::size_t total_messages = 0;

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
            metas = client.listMessages(folder.raw_name);
        } catch (const imap::ImapError& e) {
            report(log, "warning: " + folder.full_name + ": " + e.what());
            continue;
        }

        total_messages += metas.size();

        for (std::size_t start = 0; start < metas.size();) {
            // Take a batch, but stop early once the bodies it names would add up
            // to more than the byte budget: one round trip is not worth holding
            // a gigabyte of mail in memory.
            std::vector<imap::MessageMeta> batch;
            std::size_t bytes = 0;
            while (start + batch.size() < metas.size() && batch.size() < options.batch_size) {
                const auto& m = metas[start + batch.size()];
                if (!batch.empty() && bytes + m.size > options.batch_bytes) break;
                bytes += m.size;
                batch.push_back(m);
            }
            start += batch.size();

            // Anything already spooled is served from disk, and only the rest
            // is asked of the server.
            std::vector<RawMessage> raws(batch.size());
            std::vector<imap::MessageMeta> wanted;
            std::vector<std::size_t> wanted_at;
            for (std::size_t i = 0; i < batch.size(); ++i) {
                if (!options.spool_dir.empty()) {
                    const auto path = spoolPath(options.spool_dir, folder.full_name,
                                                batch[i].uid);
                    std::string cached = readFile(path);
                    if (!cached.empty()) {
                        raws[i].uid = batch[i].uid;
                        raws[i].flags = batch[i].flags;
                        raws[i].keywords = batch[i].keywords;
                        raws[i].internal_date = batch[i].internal_date;
                        raws[i].rfc822 = std::move(cached);
                        ++stats.reused_messages;
                        continue;
                    }
                }
                wanted.push_back(batch[i]);
                wanted_at.push_back(i);
            }

            if (!wanted.empty()) {
                std::vector<RawMessage> got;
                try {
                    got = client.fetchMessages(folder.raw_name, wanted);
                } catch (const imap::ImapError& e) {
                    report(log, "warning: " + folder.full_name + ": batch fetch failed (" +
                                    e.what() + "); falling back to one message at a time");
                    got.assign(wanted.size(), RawMessage{});
                }
                for (std::size_t i = 0; i < wanted.size(); ++i) {
                    // A batch can come back short -- a server may omit a message
                    // it cannot read -- so anything missing is asked for alone.
                    if (got.size() == wanted.size() && !got[i].rfc822.empty()) {
                        raws[wanted_at[i]] = std::move(got[i]);
                    } else {
                        try {
                            raws[wanted_at[i]] = client.fetchMessage(folder.raw_name, wanted[i]);
                        } catch (const imap::ImapError& e) {
                            ++stats.failed_messages;
                            report(log, "warning: UID " + std::to_string(wanted[i].uid) +
                                            ": " + e.what());
                            continue;
                        }
                    }
                    stats.fetched_bytes += raws[wanted_at[i]].rfc822.size();
                    if (!options.spool_dir.empty() && !raws[wanted_at[i]].rfc822.empty()) {
                        writeFile(spoolPath(options.spool_dir, folder.full_name,
                                            wanted[i].uid),
                                  raws[wanted_at[i]].rfc822);
                    }
                }
            }

            for (std::size_t i = 0; i < batch.size(); ++i) {
                if (raws[i].rfc822.empty()) continue;
                Message msg = mime::parse(raws[i]);
                if (batch[i].size) msg.size = batch[i].size;
                writer.addMessage(id, msg);
                ++stats.messages;
                stats.attachments += msg.attachments.size();
                if (options.progress_every && stats.messages % options.progress_every == 0) {
                    const auto now = std::chrono::steady_clock::now();
                    const double secs =
                        std::chrono::duration<double>(now - started).count();
                    std::ostringstream os;
                    os << "  " << stats.messages << "/" << total_messages << " message(s), "
                       << humanBytes(stats.fetched_bytes) << " fetched";
                    if (secs > 0.0) {
                        os << ", " << static_cast<int>(stats.messages / secs) << "/s";
                    }
                    report(log, os.str());
                }
            }
        }
        report(log, "  " + folder.full_name + ": " + std::to_string(metas.size()) +
                        " message(s)");
    }

    writer.finish();
    return stats;
}

}  // namespace imap2pst

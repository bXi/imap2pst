#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "imap/imap_client.h"
#include "pipeline/pipeline.h"

namespace {

void usage(const char* argv0) {
    std::cerr <<
        "Usage: " << argv0 << " --host HOST --user USER --output FILE.pst [options]\n"
        "\n"
        "Connection:\n"
        "  --host HOST            IMAP server hostname (required)\n"
        "  --port N               Port; defaults to 993 with TLS, 143 without\n"
        "  --user USER            Login name (required)\n"
        "  --password PASS        Password; prefer --password-env\n"
        "  --password-env VAR     Read the password from environment variable VAR\n"
        "  --oauth2-bearer TOKEN  Use an XOAUTH2 bearer token instead of a password\n"
        "  --no-tls               Plain IMAP with opportunistic STARTTLS\n"
        "  --insecure             Do not verify the server certificate\n"
        "  --timeout N            Per-request timeout in seconds (default 120)\n"
        "\n"
        "  --retries N            Attempts per request before giving up (default 3)\n"
        "\n"
        "Selection and output:\n"
        "  --output FILE.pst      Destination PST file (required)\n"
        "  --folder NAME          Migrate only this folder; repeatable\n"
        "\n"
        "Throughput and restarts:\n"
        "  --batch N              Messages per fetch round trip (default 50)\n"
        "  --batch-bytes N        Cap on one batch's message source (default 32M)\n"
        "  --spool DIR            Keep fetched message source here and reuse it,\n"
        "                         so a re-run does not download it again\n"
        "  --progress N           Report every N messages (default 100, 0 off)\n"
        "  --quiet                Only report the final summary\n"
        "  --verbose              Log progress and libcurl traffic\n";
}

bool needsValue(int i, int argc, const char* flag) {
    if (i + 1 < argc) return true;
    std::cerr << "error: " << flag << " needs a value\n";
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    imap2pst::imap::ImapConfig cfg;
    imap2pst::PipelineOptions opts;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() { return std::string(argv[++i]); };

        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else if (arg == "--host") {
            if (!needsValue(i, argc, "--host")) return 2;
            cfg.host = value();
        } else if (arg == "--port") {
            if (!needsValue(i, argc, "--port")) return 2;
            cfg.port = std::atoi(value().c_str());
        } else if (arg == "--user") {
            if (!needsValue(i, argc, "--user")) return 2;
            cfg.username = value();
        } else if (arg == "--password") {
            if (!needsValue(i, argc, "--password")) return 2;
            cfg.password = value();
        } else if (arg == "--password-env") {
            if (!needsValue(i, argc, "--password-env")) return 2;
            const char* env = std::getenv(value().c_str());
            if (!env) {
                std::cerr << "error: environment variable is not set\n";
                return 2;
            }
            cfg.password = env;
        } else if (arg == "--oauth2-bearer") {
            if (!needsValue(i, argc, "--oauth2-bearer")) return 2;
            cfg.oauth2_bearer = value();
        } else if (arg == "--no-tls") {
            cfg.use_tls = false;
        } else if (arg == "--insecure") {
            cfg.verify_peer = false;
        } else if (arg == "--timeout") {
            if (!needsValue(i, argc, "--timeout")) return 2;
            cfg.timeout_seconds = std::atol(value().c_str());
        } else if (arg == "--output") {
            if (!needsValue(i, argc, "--output")) return 2;
            opts.output_path = value();
        } else if (arg == "--folder") {
            if (!needsValue(i, argc, "--folder")) return 2;
            opts.folders.push_back(value());
        } else if (arg == "--retries") {
            if (!needsValue(i, argc, "--retries")) return 2;
            cfg.retry_attempts = std::atoi(value().c_str());
        } else if (arg == "--batch") {
            if (!needsValue(i, argc, "--batch")) return 2;
            opts.batch_size = std::strtoul(value().c_str(), nullptr, 10);
            if (opts.batch_size == 0) opts.batch_size = 1;
        } else if (arg == "--batch-bytes") {
            if (!needsValue(i, argc, "--batch-bytes")) return 2;
            opts.batch_bytes = std::strtoull(value().c_str(), nullptr, 10);
        } else if (arg == "--spool") {
            if (!needsValue(i, argc, "--spool")) return 2;
            opts.spool_dir = value();
        } else if (arg == "--progress") {
            if (!needsValue(i, argc, "--progress")) return 2;
            opts.progress_every = std::strtoul(value().c_str(), nullptr, 10);
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--verbose") {
            opts.verbose = true;
            cfg.verbose = true;
        } else {
            std::cerr << "error: unknown option " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (cfg.host.empty() || cfg.username.empty() || opts.output_path.empty()) {
        usage(argv[0]);
        return 2;
    }

    try {
        auto transport = std::make_shared<imap2pst::imap::CurlTransport>(cfg);
        imap2pst::imap::ImapClient client(transport);
        // Progress goes to stderr by default: a migration of any size is long
        // enough that silence is indistinguishable from a hang.
        std::function<void(const std::string&)> log;
        if (!quiet) {
            log = [](const std::string& m) { std::cerr << m << "\n"; };
        } else {
            opts.progress_every = 0;
        }
        const auto stats = imap2pst::run(client, opts, log);
        std::cout << "Wrote " << opts.output_path << ": " << stats.folders
                  << " folder(s), " << stats.messages << " message(s), "
                  << stats.attachments << " attachment(s)";
        if (stats.reused_messages) {
            std::cout << ", " << stats.reused_messages << " from the spool";
        }
        if (stats.failed_messages) {
            std::cout << ", " << stats.failed_messages << " failed";
        }
        std::cout << "\n";

        // Name what was left behind, so the messages can be found on the server
        // afterwards rather than only counted here.
        if (!stats.failed_uids.empty()) {
            std::cerr << "\nThese messages were skipped:\n";
            std::size_t shown = 0;
            for (const auto& f : stats.failed_uids) {
                if (shown++ == 20) {
                    std::cerr << "  ... and " << (stats.failed_uids.size() - 20)
                              << " more\n";
                    break;
                }
                std::cerr << "  " << f.folder << " UID " << f.uid << ": " << f.reason
                          << "\n";
            }
        }
        return stats.failed_messages ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

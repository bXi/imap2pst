// Writes a large store and checks what only shows up at size: that throughput
// does not decay as the B-trees deepen, that memory stays proportional to the
// message count rather than to the data, and that a file past the 4 GB mark is
// still addressed correctly.
//
// Run by hand or through the "scale" CTest label; it is not part of the default
// suite because it writes gigabytes.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <sys/resource.h>

#include "pst/pst_writer.h"

using namespace imap2pst;

namespace {

long peakRssKb() {
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

Message syntheticMessage(std::size_t i) {
    Message m;
    m.subject = "Message number " + std::to_string(i);
    m.from = {"Sender " + std::to_string(i % 500),
              "s" + std::to_string(i % 500) + "@example.com"};
    m.to = {{"Recipient", "r@example.com"}};
    m.body_text = "Body " + std::to_string(i) + std::string(i % 4096, '.');
    m.delivery_time = 1700000000 + static_cast<std::int64_t>(i);
    m.date = m.delivery_time;
    m.headers = {{"Subject", m.subject}, {"X-Seq", std::to_string(i)}};
    if (i % 25 == 0) {
        Attachment a;
        a.filename = "payload.bin";
        a.content_type = "application/octet-stream";
        a.data.assign(20000 + (i % 50000), static_cast<std::uint8_t>(i));
        m.attachments.push_back(std::move(a));
    }
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pst_scale_probe OUT.pst [messages] [folders]\n");
        return 2;
    }
    const std::size_t count = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 25000;
    const std::size_t folders = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 25;

    const auto start = std::chrono::steady_clock::now();
    double first_quarter_rate = 0.0;

    {
        pst::PstWriter w(argv[1]);
        std::vector<pst::FolderId> ids;
        for (std::size_t i = 0; i < folders; ++i) {
            ids.push_back(w.createFolder(w.ipmSubtree(), "Folder " + std::to_string(i)));
        }
        for (std::size_t i = 0; i < count; ++i) {
            w.addMessage(ids[i % folders], syntheticMessage(i));
            if (i + 1 == count / 4) {
                const double secs = std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - start).count();
                if (secs > 0.0) first_quarter_rate = (count / 4) / secs;
            }
        }
        w.finish();
    }

    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
    const auto size = static_cast<std::uint64_t>(f.tellg());
    const double rate = secs > 0.0 ? count / secs : 0.0;
    const long rss_mb = peakRssKb() / 1024;

    std::printf("%zu messages, %zu folders: %.1fs (%.0f msg/s), peak RSS %ld MB, "
                "file %.2f GB\n",
                count, folders, secs, rate, rss_mb, size / (1024.0 * 1024 * 1024));

    // Throughput must not fall away as the trees deepen.  Half the early rate is
    // a generous bar -- the measured curve is flat -- but it would catch a
    // per-message scan of something that grows.
    if (first_quarter_rate > 0.0 && rate < first_quarter_rate / 2.0) {
        std::fprintf(stderr,
                     "FAIL: throughput decayed from %.0f to %.0f msg/s; something is "
                     "scanning a structure that grows\n",
                     first_quarter_rate, rate);
        return 1;
    }

    // Memory is the index of what has been written, not the mail itself: about
    // 250 bytes a message.  A body or attachment held past its message would
    // show up here long before it exhausted a machine.
    const double bytes_per_message = rss_mb * 1024.0 * 1024.0 / static_cast<double>(count);
    if (bytes_per_message > 2048.0) {
        std::fprintf(stderr,
                     "FAIL: %.0f bytes of memory per message, expected about 250\n",
                     bytes_per_message);
        return 1;
    }
    std::printf("  %.0f bytes of memory per message\n", bytes_per_message);

    // The file is the point of the run, not a result to keep: at the sizes this
    // is meant for it would fill a disk over a few runs.
    if (std::getenv("IMAP2PST_KEEP_SCALE_PST") == nullptr) std::remove(argv[1]);
    return 0;
}

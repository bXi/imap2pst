// Writes the smallest PST this writer can produce: no attachments, no
// unrecognised headers, and therefore no named properties of its own.
//
// That last part is the point. A store whose name-to-id map ends up empty is
// unreadable, and every other fixture here has headers that mint named
// properties, so nothing else in the suite would notice.

#include <iostream>

#include "pst/pst_writer.h"

using namespace imap2pst;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: pst_minimal_writer OUT.pst\n";
        return 2;
    }
    pst::PstWriter w(argv[1]);
    const auto inbox = w.createFolder(w.ipmSubtree(), "Inbox");

    Message m;
    m.subject = "Plain";
    m.from = {"Alice Example", "alice@example.com"};
    m.to = {{"Bob Builder", "bob@example.com"}};
    m.body_text = "Body.\n";
    m.date = m.delivery_time = 1700000000;
    m.flags = kFlagSeen;
    // Only headers that map to structured properties, so none become named.
    m.headers = {{"Subject", m.subject},
                 {"From", "Alice Example <alice@example.com>"},
                 {"To", "Bob Builder <bob@example.com>"}};
    w.addMessage(inbox, m);
    w.finish();
    return 0;
}

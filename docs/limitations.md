# Known limitations

What this tool does not do, and what it does approximately. Read this before
pointing it at a mailbox you care about.

This milestone favours correctness over completeness. In rough order of how
likely you are to hit them:

**Format scope**

* **Unicode PST only.** The ANSI/32-bit layout is not written and not read.
* **No encryption.** Files are written with `NDB_CRYPT_NONE`. The permute and
  cyclic encodings are not implemented.
* **Append-only allocation.** Space is handed out by a bump allocator that steps
  over the reserved pages. Nothing is ever freed or reused, and there is no
  compaction, so rewriting is not possible and the file is larger than an
  Outlook-written equivalent.
* **No FMap/FPMap pages.** They are marked absent in the header, which
  [MS-PST] permits for Unicode files. A PMap page is written per span and marked
  fully allocated.
* **Two levels of data tree.** XBLOCK and XXBLOCK are both implemented, which
  caps a single property value at roughly 8 GB — far past anything that matters,
  but it is a cap, not an arbitrary limit.
* **Named properties cap at 4096** distinct names per file. Beyond that, headers
  are still preserved in `PidTagTransportMessageHeaders`; they just do not also
  get their own addressable property.

**Fidelity**

* **RTF only for plain-text messages.** A message with no HTML body gets an
  `PidTagRtfCompressed` body generated from its text. One with HTML does not:
  Outlook derives a better RTF body from the HTML, and a competing one written
  here would be the version it displays. The stream is written **uncompressed**,
  which is the only form Outlook renders: every compressed variant tried --
  the format's own weak CRC, a standard CRC-32, a trailing end-of-stream token,
  a different codepage -- produced `<<Error: data corruption>>` in place of the
  body. libpff is the opposite, and cannot read the uncompressed form at all,
  because it runs its LZ decoder whichever signature it finds. There is no form
  both accept, so Outlook wins: a body a person cannot read is the failure that
  matters, and libpff still has `PidTagBody`.
* **Message class follows the content type.** Delivery reports and signed mail
  are classed as such; anything else is `IPM.Note`. Calendar items, contacts and
  tasks arriving over IMAP are still stored as ordinary mail.
* **Flags.** `\Seen` becomes `MSGFLAG_READ`, `\Flagged` becomes
  `PidTagFlagStatus`, `\Draft` becomes `MSGFLAG_UNSENT`, and `\Answered` sets
  the last verb, which is where the reply arrow in Outlook's message list comes
  from. Other IMAP keywords become Outlook categories.
* **Embedded messages are one level of fidelity down.** A `message/rfc822` part
  is written as a message inside its attachment, with its own recipients and
  attachments, so Outlook opens it in place. The original source is kept as
  well, so nothing is lost. Nesting deeper than eight levels stays a plain
  attachment.
* **No search folders, no associated (FAI) content.** The associated contents
  table exists on every folder but is always empty.
* **The name-to-id hash buckets are written on a best guess.** libpff and
  java-libpst both resolve named properties from the entry stream and ignore the
  buckets, so a disagreement with Outlook's exact hash would not show up in the
  test suite. See the comment at the top of `src/pst/nameid_map.h`.
* **Every message gets an attachment table**, even with no attachments. It costs
  one small node per message and keeps libpff's subnode lookup on its happy
  path; see the comment in `PstWriter::addMessage`.

**Outlook**

Verified against a real mailbox: 13 folders and 147 messages migrated from an
IMAP server open directly in Outlook (build 16.0.10417.20207) -- no repair pass,
no damage report -- with folder hierarchy, nested folders, message bodies,
attachments, and non-ASCII subjects, sender names and folder names intact.

Two of the fixes are worth knowing about before touching this code. The first is
in the LTP layer: **string-named properties are matched without regard to case**,
so `Content-Type` and `Content-type` are one property and must share one id.
Minting an id per spelling leaves Outlook holding two map entries for a single
name; it opens the store but reports it as damaged, and the Inbox Repair Tool
dereferences the entry it could not resolve and crashes. Header spelling varies
freely in real mail -- one `Content-type` among 147 messages was enough.

That one is a lesson in what a bisect can and cannot tell you. The failure
tracked nothing about the message that triggered it: not its content, not its
folder, not file size, not B-tree depth. Halving the mailbox produced two halves
that both passed. What identified it was counting name-to-id entries across every
file already judged: every good one had 89 or fewer, every bad one exactly 90.
A property of the whole store, invisible in any single message, so no amount of
narrowing down to "the 68th message" was going to name it.

The second is in the NDB layer: **a page's absolute file offset must be a multiple
of 512**. Blocks are allocated in 64-byte units, so a page written straight
after one lands misaligned unless the cursor is advanced first. Outlook
fail-fasts out of `mspst32.dll` with HRESULT `0x80040813` and the internal
message "Page has misaligned or zero ib". Nothing else detects it: libpff reads
such a file perfectly, the repair tool rebuilds both B-trees rather than
reporting the offset, and the page contents are entirely valid -- only their
position is wrong, which no structural comparison looks at.

**Transport**

* **Plain `LOGIN` is the tested path.** `--oauth2-bearer` wires up libcurl's
  XOAUTH2 support but has not been exercised against a real provider.
* **A run cannot resume into a half-written PST.** `--spool` makes a repeat run
  cheap by keeping the fetched message source, so only what was not yet
  downloaded is fetched again, but the PST itself is always rebuilt from the
  start. An interrupted run's output file is discarded.
* **No incremental migration.** There is no notion of "what changed since last
  time": every run writes the whole mailbox.

---

Back to the [README](../README.md).

[MS-PST]: https://learn.microsoft.com/en-us/openspecs/office_file_formats/ms-pst/

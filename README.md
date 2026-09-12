# imap2pst

Server-side IMAP → PST migration. Conceptually `imapsync`, except the
destination is a Microsoft PST file rather than another IMAP server.

It runs headless on Linux. There is no dependency on Outlook, MAPI, Windows or
any commercial component: the PST is written directly, from the published
[MS-PST] file format.

This is the **first milestone**: a complete pipeline — connect, fetch, parse,
write — that is correct end to end but deliberately narrow. See
[Known limitations](#known-limitations) before pointing it at anything you care
about.

---

## What it does

```
IMAP server ──libcurl──▶ raw RFC 822 ──vmime──▶ Message ──▶ Unicode PST
             LIST                      MIME              NDB / LTP
             UID FETCH                 charsets          folders, messages,
             FLAGS, INTERNALDATE       attachments       attachments
```

* **`src/imap/`** — folder listing and message fetching over `imap://` /
  `imaps://`, using libcurl's native IMAP support. Response *parsing* is split
  from *transport* (`imap_parse.h` vs `imap_client.h`) so the test suite drives
  it with recorded fixtures instead of a live mailbox.
* **`src/mime/`** — vmime turns raw RFC 822 octets into a normalized
  `Message`: structured headers, plain and HTML bodies, attachments, and a
  catch-all list of every original header so nothing is silently lost.
  Everything it emits is UTF-8.
* **`src/pst/`** — the PST writer, layered the way the spec is: `ndb.*` (blocks,
  allocation maps, the two B-trees), `heap.*` (heap-on-node, B-tree-on-heap),
  `ltp.*` (property and table contexts), `nameid_map.*` (named properties), and
  `pst_writer.*` on top mapping a `Message` to MAPI properties.
* **`src/pipeline/`** — glue, plus `src/main.cpp` for the CLI.

The PST module depends on neither libcurl nor vmime, so it can be hardened and
extended on its own.

---

## Building

Requirements:

* CMake ≥ 3.20, a C++17 compiler (tested with GCC 13)
* OpenSSL and zlib development headers (for libcurl's TLS)
* Network access on the first configure, to fetch dependencies

libcurl, vmime and GoogleTest are pulled in with `FetchContent` and pinned to
exact revisions in `cmake/Dependencies.cmake`. Nothing is vendored into the
tree. An already-installed libcurl is used when CMake can find one.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

The first build compiles curl and vmime from source and takes a few minutes;
later builds do not.

Useful options:

| Option | Default | Effect |
| --- | --- | --- |
| `IMAP2PST_BUILD_TESTS` | `ON` | Build the test suite (needs GoogleTest) |
| `IMAP2PST_WITH_IMAP` | `ON` | Build the libcurl transport |
| `IMAP2PST_WITH_MIME` | `ON` | Build the vmime parser |

Turning both `IMAP2PST_WITH_IMAP` and `IMAP2PST_WITH_MIME` off builds just the
PST writer and its tests, with no third-party dependency except GoogleTest —
handy when iterating on the format layer.

---

## Running it against a real account

```sh
build/src/imap2pst \
  --host imap.example.com \
  --user alice@example.com \
  --password-env IMAP_PASSWORD \
  --output alice.pst \
  --verbose
```

`--password-env` names an environment variable rather than putting the secret
in the command line, where it would be visible in `ps` and shell history.
`--password` exists for scripted use but prefer the former.

Options:

```
--host HOST            IMAP server hostname (required)
--port N               Port; defaults to 993 with TLS, 143 without
--user USER            Login name (required)
--password PASS        Password (visible in the process list; prefer --password-env)
--password-env VAR     Read the password from environment variable VAR
--oauth2-bearer TOKEN  XOAUTH2 bearer token instead of a password
--no-tls               Plain IMAP, with opportunistic STARTTLS
--insecure             Skip server certificate verification
--timeout N            Per-request timeout in seconds (default 120)
--retries N            Attempts per request before giving up (default 3)
--output FILE.pst      Destination PST (required)
--folder NAME          Migrate only this folder; repeat for several
--batch N              Messages per fetch round trip (default 50)
--batch-bytes N        Cap on one batch's message source (default 32M)
--spool DIR            Keep fetched message source here and reuse it
--progress N           Report every N messages (default 100, 0 off)
--quiet                Only print the final summary
--verbose              Log progress, and libcurl's own protocol trace
```

Bodies are fetched a batch at a time -- one `UID FETCH` naming up to `--batch`
messages, rather than one request each. The batch is held in memory while it is
parsed, so `--batch-bytes` closes it early when the messages it names turn out
to be large; a folder of 50 MB attachments does not become a 50 MB batch.

`--spool DIR` keeps each message's source on disk and reuses it on a later run,
which is what makes an interrupted migration cheap to restart: the second run
pays only for what the first had not yet downloaded. The PST is always rebuilt
from scratch, so the restart is not resuming a half-written file -- it is
skipping the download. The spool is plain RFC 822 source, one file per message,
and can be deleted at any time.

Folder hierarchy is preserved. The server's `LIST` delimiter is used to split
names, so `INBOX.Work.2024` on a Courier-style server becomes
`Inbox → Work → 2024` inside the PST, under `Top of Personal Folders`.

Exit status is `0` on success, `1` if some messages could not be fetched (the
PST is still written with the rest), `2` on a usage error.

---

## Testing

```sh
cd build && ctest --output-on-failure
```

Four groups run, none of which touches the network:

**Unit tests.** IMAP response parsing is driven by recorded fixtures in
`tests/fixtures/*.txt`; MIME normalization by `.eml` fixtures covering plain
text, multipart with attachments, HTML-only, ISO-8859-1, and UTF-8 with RFC 2047
encoded headers.

**Client and pipeline tests.** `ImapTransport` is an interface, so
`test_imap_client.cpp` and `test_pipeline.cpp` replace it with a fake that
replays fixture responses. The pipeline test runs the whole chain — recorded
IMAP traffic through vmime into a PST — and re-reads the result, so folder
hierarchy, message counts and the folder filter are covered without a mailbox.

**PST binary structure.** `tests/test_pst_binary.cpp` writes PSTs and re-reads
them with a small parser written straight from the spec — not shared with the
writer — asserting the header magic and CRCs, page and block trailers and
signatures, B-tree ordering and depth, that no block overlaps a reserved AMap
or PMap page, and that the allocation map marks every block that exists.

**Round-trip against libpff.** `tests/pst_fixture_writer.cpp` writes a PST plus
a JSON manifest of exactly what went in; `tests/scripts/verify_pst.py` reads the
PST back through `pypff` and checks folder names, subjects, bodies, attachment
bytes and the named-property map against that manifest. libpff shares no code
with this project, so it is a genuine oracle rather than a mirror.

### Test-only prerequisite: pypff

The round-trip test needs `pypff`, the Python binding for libpff. It is **not**
a build dependency and nothing links against it — CMake only needs a Python 3
interpreter to invoke the script.

```sh
python3 -m venv .venv
.venv/bin/pip install libpff-python
cmake -S . -B build -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

`-DPython3_EXECUTABLE` is only needed when the interpreter holding `pypff` is
not the default `python3`.

When `pypff` is missing the script exits `77` and CTest reports the test as
**skipped**, not failed, so the rest of the suite still runs. Distribution
packages (`python3-libpff`) work equally well, as does shelling out to
`pffexport` by hand for a spot check.

### Opt-in live server test

There is one test that does need a real account. It is off by default and never
runs in CI:

```sh
cmake -S . -B build -DIMAP2PST_LIVE_TEST=ON
cmake --build build -j
IMAP2PST_HOST=imap.example.com \
IMAP2PST_USER=alice@example.com \
IMAP2PST_PASSWORD='...' \
IMAP2PST_FOLDER=INBOX \
ctest --test-dir build -R imap_live_migration --output-on-failure
```

It migrates one folder, asserts a non-empty PST came out, and — when `pypff` is
present — reads every message back through libpff.

---

## Known limitations

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

* **No RTF body.** `PidTagRtfCompressed` is not written; bodies are stored as
  `PidTagBody` and `PidTagHtml` with `PidTagInternetCodepage` set to UTF-8.
* **Message class is always `IPM.Note`.** Calendar items, contacts and tasks
  arriving over IMAP are stored as ordinary mail.
* **Read state and the flagged flag only.** `\Seen` becomes `MSGFLAG_READ`,
  `\Flagged` becomes `PidTagFlagStatus`. Other IMAP keywords survive on the
  in-memory `Message` but are not yet written to the PST.
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

That defect is also a lesson in how it presents. Whether a page happened to land
aligned depended on how many blocks preceded it, so the failure tracked folder
count and placement while following no rule that made sense: one folder under an
empty parent worked, two did not; a subtree with three children worked, two or
four did not. Hours went into hypotheses about child counts and hierarchy
tables. The answer came from attaching a debugger, breaking on the exception,
and reading the format string beside the error code -- worth doing early once a
failure proves deterministic.

Two further cautions. The repair tool's complaints are advisory -- it repairs the
file regardless -- and acting on them twice broke a configuration that
previously opened. And a PST that Outlook has opened is no longer the file that
was written, because it adds its own search folders on first open; diffing one of
those measures the wrong thing.

Not written, and not required for Outlook to open a store cleanly: the search
folders and their update queues, and node `0xEE1`, a flat list with one record
per object that Outlook builds for itself. Writing `0xEE1` in Outlook's exact
format stops even a bare store from opening, so its payload carries meaning this
writer does not understand -- see the comment in
`PstWriter::writeReservedNodes`.

**Transport**

* **Plain `LOGIN` is the tested path.** `--oauth2-bearer` wires up libcurl's
  XOAUTH2 support but has not been exercised against a real provider.
* **One message per request.** Bodies are fetched with an individual
  `UID FETCH` per message rather than pipelined, so large mailboxes are slower
  than they need to be.
* **No resume.** An interrupted run leaves a partial PST that must be discarded.

---

## Layout

```
cmake/Dependencies.cmake   pinned FetchContent declarations
src/core/                  the normalized Message every module speaks
src/imap/                  imap_parse.*  response parsers (no I/O)
                           imap_client.* libcurl transport + folder walking
src/mime/                  vmime-backed normalization
src/pst/                   pst_format.h  on-disk constants and helpers
                           crc.*         the [MS-PST] weak CRC-32
                           ndb.*         blocks, allocation maps, NBT/BBT
                           heap.*        heap-on-node, B-tree-on-heap
                           ltp.*         property and table contexts
                           nameid_map.*  named properties, node 0x61
                           prop_tags.h   the MAPI tags this writer emits
                           pst_writer.*  Message -> folders and messages
src/pipeline/              IMAP -> MIME -> PST
tests/                     unit tests, fixtures, round-trip writer and script
```

## References

* [MS-PST], *Outlook Personal Folders (.pst) File Format* — the primary
  reference for everything under `src/pst/`.
* [java-libpst](https://github.com/rjohnsondev/java-libpst) — used as a second
  opinion where the spec is ambiguous, particularly around the NDB allocation
  maps and the heap-on-node and property-context layers.
* [libpff](https://github.com/libyal/libpff) — read-only, used as the test
  oracle.

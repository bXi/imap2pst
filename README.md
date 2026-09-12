# imap2pst

[![Ko-fi](https://img.shields.io/badge/Ko--fi-buy%20me%20a%20coffee-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/bixxy)

**Migrate an IMAP mailbox into an Outlook PST file, from Linux, with no Outlook
and no Windows involved.**

Conceptually [`imapsync`](https://imapsync.lamiral.info/), except the
destination is a Microsoft PST file rather than another IMAP server. The PST is
written directly from the published [MS-PST] format, so nothing here needs
Outlook, MAPI, a Windows machine or a commercial library.

```sh
imap2pst --host imap.example.com \
         --user alice@example.com \
         --password-env IMAP_PASSWORD \
         --output alice.pst
```

```
Listing folders
Fetching INBOX
  100/1420 message(s), 84.2 MB fetched, 312/s
  ...
Wrote alice.pst: 14 folder(s), 1420 message(s), 96 attachment(s)
```

Open the result in Outlook with **File → Open & Export → Open Outlook Data
File**.

---

## Status

Verified against a real mailbox: Outlook opens the output directly, with no
repair prompt and no damage report. The writer has been run to a million
messages and a 10 GB file.

What it preserves: folder hierarchy including nested and non-ASCII folder
names, plain and HTML bodies, attachments, inline images, embedded (forwarded)
messages, read and answered state, and IMAP keywords as Outlook categories.

What it does not do yet: calendar items, contacts and tasks arrive as ordinary
mail; there is no ANSI PST, no encryption, and no space reuse. The full list is
in **[docs/limitations.md](docs/limitations.md)** — worth reading before a
migration that matters.

---

## Install

Packages are attached to each [release](../../releases): a `.deb` for Ubuntu
22.04 and 24.04, an `.rpm` for Fedora, and a portable tarball.

```sh
sudo apt install ./imap2pst_0.1.0_amd64~ubuntu24.04.deb
```

### Build from source

Needs a C++17 compiler, CMake 3.20+, and OpenSSL, zlib and ICU development
headers. libcurl is fetched and built automatically unless the system already
has it.

```sh
sudo apt install build-essential cmake ninja-build libssl-dev zlib1g-dev libicu-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
build/src/imap2pst --help
```

The first build compiles libcurl from source if the system has no development
package, which takes a couple of minutes; later builds do not.

| Option | Default | Effect |
|---|---|---|
| `IMAP2PST_BUILD_TESTS` | `ON` | Build the test suite |
| `IMAP2PST_USE_SYSTEM_DEPS` | `OFF` | Require the system libcurl rather than building one |
| `IMAP2PST_WITH_IMAP` | `ON` | Build the libcurl IMAP transport |
| `IMAP2PST_WITH_MIME` | `ON` | Build the MIME parser |

`cpack` in the build directory produces the packages.

---

## Usage

```
Connection:
  --host HOST            IMAP server hostname (required)
  --port N               Port; defaults to 993 with TLS, 143 without
  --user USER            Login name (required)
  --password PASS        Password (visible in the process list)
  --password-env VAR     Read the password from this environment variable
  --oauth2-bearer TOKEN  XOAUTH2 bearer token instead of a password
  --no-tls               Plain IMAP, with opportunistic STARTTLS
  --insecure             Skip server certificate verification
  --timeout N            Per-request timeout in seconds (default 120)
  --retries N            Attempts per request before giving up (default 3)

Selection and output:
  --output FILE.pst      Destination PST (required)
  --folder NAME          Migrate only this folder; repeat for several

Throughput and restarts:
  --batch N              Messages per fetch round trip (default 50)
  --batch-bytes N        Cap on one batch's message source (default 32M)
  --spool DIR            Keep fetched message source here and reuse it
  --progress N           Report every N messages (default 100, 0 off)
  --quiet                Only print the final summary
  --verbose              Log progress, and libcurl's own protocol trace
```

Exit status is `0` when everything migrated, `1` when the run finished but some
messages could not be fetched or parsed — they are listed by folder and UID —
and `2` for a bad command line.

### Passwords

Use `--password-env`. `--password` puts the secret in the process list where
any user on the machine can read it.

### Resuming an interrupted run

`--spool DIR` keeps each message's source on disk and reuses it next time, so a
second run pays only for what the first had not yet downloaded:

```sh
imap2pst --host imap.example.com --user alice@example.com \
         --password-env IMAP_PASSWORD \
         --spool /var/cache/imap2pst --output alice.pst
```

The PST is always rebuilt from scratch — the restart skips the *download*, it
does not resume a half-written file. The spool holds the whole mailbox in plain
text; delete it when the migration is done.

### Large mailboxes

Bodies are fetched in batches rather than one request per message, so the run is
bounded by the server rather than by round trips. Memory stays proportional to
the number of messages (about 250 bytes each), not to the size of the mail: a
million-message mailbox needs roughly 260 MB.

---

## How it works

```
IMAP server ──libcurl──▶ raw RFC 822 ──our MIME──▶ Message ──▶ Unicode PST
             LIST                      parser              NDB / LTP
             UID FETCH                 charsets            folders, messages,
             FLAGS, INTERNALDATE       attachments         attachments
```

* **`src/imap/`** — folder listing and message fetching over `imap://` and
  `imaps://`. Parsing is split from transport so tests drive it with recorded
  responses instead of a live mailbox.
* **`src/mime/`** — turns raw RFC 822 into a normalized `Message`: headers,
  addresses, the multipart tree, transfer encodings and charsets, everything
  transcoded to UTF-8. This is the project's own code rather than a library, so
  that no dependency dictates the licence.
* **`src/pst/`** — the PST writer, layered as the specification is: blocks and
  B-trees, heap-on-node, property and table contexts, named properties, and the
  mapping from `Message` to MAPI properties on top.
* **`src/pipeline/`** — glue, plus `src/main.cpp` for the CLI.

The PST module depends on neither libcurl nor the MIME layer, so it can be
tested and extended on its own. More in **[docs/internals.md](docs/internals.md)**.

---

## Development

```sh
cmake -S . -B build -G Ninja
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The suite needs no network and no IMAP server. `pip install libpff-python`
enables the round-trip test, which reads every generated PST back with libpff —
a reader sharing no code with the writer. Without it that test is skipped.

Testing approach, the scale probe, and what it took to make Outlook accept the
output are in [docs/internals.md](docs/internals.md).

---

## Dependencies

All permissive: nothing here constrains what licence this project may carry.

| Dependency | Licence | Used for |
|---|---|---|
| libcurl | MIT-like | IMAP transport |
| ICU | Unicode licence | Charset conversion |
| OpenSSL | Apache-2.0 | TLS, via libcurl |
| zlib | zlib licence | Compression, via libcurl |
| GoogleTest | BSD-3 | Tests only |

MIME parsing is this project's own code rather than a library, which is what
keeps that list free of copyleft. See
[docs/internals.md](docs/internals.md#mime-parsing) for how it is built and
tested.

## Support

If imap2pst saved you an afternoon, you can
[buy me a coffee on Ko-fi](https://ko-fi.com/bixxy).

## Licence

**Not yet chosen.** No dependency constrains the choice — pick whatever suits
you and add a `LICENSE` file. The release workflow refuses to publish without
one.

[MS-PST]: https://learn.microsoft.com/en-us/openspecs/office_file_formats/ms-pst/

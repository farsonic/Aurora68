# Aurora68 Play's shared core (play/core)

One code base for the logic of both Aurora68 Plays: the classic one on the Macintosh SE (`mac/piplay`, C for
the 68000, built by `mac/Makefile`) and the one for a modern Mac (`macos/Aurora68Play`, SwiftUI). Everything
that is not interface is here, in portable C, and both compile these files: the classic Play with
`m68k-linux-gnu-gcc` and its kit's flags (`mac/c`), the macOS Play with clang as the Swift package's C target
`PlayCore`, and the host tests with cc. MPL-2.0, like the rest of the Macintosh side.

The owner asked on 22 November 2026 for "a common code base between the two"; docs/08 item 107 is the record
of how it was done and what it found, docs/09 ("The shared core") the description.

## What is in it

| Part | What | Who uses it |
|---|---|---|
| `text.c` | bounded copies and appends, numbers in and out (with thousandths: no floating point on a 68000), times as "3:07", %XX escaping, a name as a record shop files it ("The Cure" under C), a 32-bit random generator the same on every machine | both |
| `names.c` | a song's number and title from its file's name ("01. Artist - Title.m4a", "1-01 Title.flac"), the stem of a path | the macOS Play (commands' tracks without tags); the tests check it agrees with the server's tags |
| `address.c` | addresses as typed (`192.168.1.20:8423`, `[fd00::20]:8423`, a bare IPv6 address, a URL), a station's URL (`http://host:port/path`, IPv6 in brackets), URLs with IPv6 in brackets and a zone as `%25`, hosts ordered by the owner's family (IPv6 first or IPv4 first), the UDP probe's answer and the order to try its addresses in | both |
| `proto.c` | the music server's protocol: every request's path (with `enc=utf8&full=1` for the macOS Play), the answers taken apart in place (lists with their `A68M` head and pages, info, artists, albums, tracks, search, covers, radio, devices, commands, the Live channel), the actions by name, the library's generation, a click on a position bar as a time, another player's position now | both |
| `catalog.c` | a page of a list as rows (the classic Play keeps 200 of them, never the library), its "previous page" and "next page" lines, a row found again by its id after the library changed, a row found by typing its name | the classic Play's Library window |
| `queue.c` | the play queue: Play Next, Add, Remove, Jump, Repeat all and one, Previous restarting a song more than three seconds in, Shuffle of what is to come, and the stream sent ahead (the classic Play pours the next song into the card before the last is heard) | both |
| `devices.c` | a Play as one of the server's devices: when to announce, when what plays is news, the long poll's path, what an answer means | both |
| `icy.c` | an internet radio station's answer: HTTP or ICY status, headers, chunks, ICY metadata and StreamTitle in Mac Roman, a playlist's first stream, the family of a MacTCP address | the classic Play's radio; the macOS Play for a station whose address is a `.pls` or `.m3u` |
| `stations.c` | the station list: the built-in one, the file's groups (the Radio menu's submenus), the defaults a file lacks | the classic Play's Radio menu; the music server's `/mac/radio` is the same list (its test checks they agree); the macOS Play's fallback for a server without it |

`media.c` (what a file is, its tags, the bytes for the card's decoders) stays in `mac/piplay`: only the classic
Play reads music files itself; the macOS Play leaves them to AVFoundation.

What stays with each Play is what is the platform's: the classic Play's Toolbox windows and menus, MacTCP and
Riptide's `.IPP6` (`fetch.c`, `radio.c`, `net.s`), the sound card's stream channel; the macOS Play's SwiftUI,
AVFoundation, URLSession and BSD sockets, and the folding of case and accents in its library's filter
(Foundation's Unicode tables).

## The wire format

One: the music server's `/mac/` text (tools/music-server/README.md, "Aurora68 Play's Library" and "Devices"),
made for a 68000 and read here with a loop. One record a line ended by a Return, fields separated by tabs; a
list starts with `A68M 1 KIND TOTAL FROM COUNT` and comes in pages (`?from=N&count=M`). Two parameters, which the
classic Play does not send, so its answers are what they were:

- `enc=utf8`: the answer in UTF-8 and the request's words read as UTF-8; without it Mac Roman both ways. The core
  moves bytes and never looks past ASCII; each Play turns them into its own text at its edge.
- `full=1`: the longer records: an album's artist id and folder, a track's path (the macOS Play plays the
  original file through `/file/`), album title and folder, a device's whole state, the path and album of a
  command's tracks, how often to announce. Paths and folders are never cut short (the 120 characters a name may
  have do not apply to them).

- `videos=1` (22 November 2026, docs/08 item 109): music videos. An album's or an artist's tracks are followed by its
  videos as `V` records (`a68_next_item` tells them from tracks, `a68_next_video` reads them alone, `a68_next_track`
  passes over them), artists count theirs, a search finds them, `/mac/info` adds the count and what the server can do
  with them, a device says whether it shows them (`a68_device.video`; `dv.video` makes a Play's announce say so), and
  a command's videos are `V` lines. The catalogue's rows have kind `'V'` (not counted in an album's length), and
  `A68_V_ARTIST` is an artist's tracks and videos, for an artist with videos and no album. `a68_path_vaudio` (the
  sound as MP3, what the classic Play plays), `a68_path_video` (the file, or HLS, what a Mac's player opens),
  `a68_path_vthumb`, `a68_path_videos` and `a68_what_video` (`v:ID`) write the requests.

The macOS Play used the server's JSON (`/api/...`) before; the JSON is still served, for the web panel and
anything else, and nothing of it changed.

## Rules the code keeps

- No header is included and no C library function is called: the classic Play's kit has `memcpy`, `memset`,
  `memmove`, `memcmp` and `strlen` (GCC calls them by itself) and nothing else. Every loop is written out.
- No allocation and no global state that two users would share: every function works on the caller's struct
  and the caller's buffers (two Library windows, or a test beside the app, could each have their own).
- Answers are taken apart in place: their tabs and Returns become NULs and each field is a pointer into the
  answer, so a field has no length of the core's choosing. The caller's buffer needs one byte more than the
  answer (a NUL goes after it).
- Big- and little-endian alike, 32- and 64-bit `long` alike: arithmetic that wraps is masked to 32 bits (the
  shuffle's generator, the device's id), so a seed gives the same order and the same id on the SE and on a Mac.
- 68000-friendly: no 64-bit arithmetic, no floating point (times in thousandths), small stacks. The classic
  Play compiles the core at `-Os` with a section per function and links with `--gc-sections`, so the parts only
  the macOS Play uses cost the SE nothing.

## Building and testing

- The classic Play: `make -C mac` (the core's objects in `mac/build/pp/core/`).
- The macOS Play: `swift test` and `swift build` at the repository's top, where `Package.swift` is (a SwiftPM
  target must lie inside its package's folder, and the classic Play's Makefile reaches the core from `mac/`, so
  the package is the repository and its targets name their folders); `macos/Aurora68Play/make-app.sh` makes the
  app. GitHub's macOS runner does both for every push that touches `play/`, `macos/` or `Package.swift`.
- The host tests, in `make -C mac check`: `tools/play_core_test.c` (every part, on answers written as the server
  writes them), the same program on the real server's answers (`tools/play_core_fixtures.py` makes a library of
  ffmpeg tones, starts `tools/music-server/aurora68_music_server.py` on it and keeps what it answers both Plays),
  the same units as a 68020 program, big-endian with 32-bit longs, under qemu-m68k (`tools/play_core_m68k.sh`),
  and the older tests of the parts that moved here (`tools/icy_test.c`, `tools/stations_test.c`,
  `tools/devices_test.c`, `tools/music-server/play_check.c` in the server's own test). The macOS Play's XCTest
  suite (`macos/Aurora68Play/Tests`) checks its Swift wrappers over the core.

Written on 22 November 2026; music videos added the same day (158 unit checks and 34 on the server's answers since,
158 as a 68020 program).

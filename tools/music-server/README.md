# The Aurora68 music server

A folder of music, organised in folders however you like, served as internet radio stations over IPv4
and IPv6. MP3s are sent as they are; FLAC, M4A (AAC and ALAC), Ogg, Opus, WAV and AIFF are converted to
MP3 as they are sent (ffmpeg, which the Docker image has; 192 kbit/s by default). Every folder is a
station: its songs and those of the folders inside it, one after another,
with each song's title, at the rate they play. Aurora68 Play on the SE plays them like any station
(Radio menu, or Radio > Open URL...), and so do VLC, Apple Music/iTunes (File > Open Stream), foobar2000,
a Squeezebox or a browser. A browser at `http://NAS:8000/` gets a page listing the folders, with each
folder's station, its files and an M3U playlist.

One Python file, standard library only, plus ffmpeg for the formats that are not MP3. It only reads the music folder, and it serves nothing outside
it, symbolic links included. It has no passwords: keep it on your home network and don't forward its
port from the internet.

## The control panel and the Live channel
`http://NAS:8423/` is a control panel: the library as cover pictures (artists, albums, songs, with a
filter box), and **Live**, a channel of your own. Choose an album, an artist, a folder or single songs
(Play, Shuffle, Add to queue, Play next) and that is what plays on `http://NAS:8423/live`, for every
listener at the same moment, like a radio station you control. The panel shows Now Playing with the
cover and the time, the queue (jump to or remove a song), Previous, Pause and Next, Repeat and
Shuffle rest, who is listening, and a "Listen here" button that plays it in the browser. **Play on**
(at the top) sends the library's buttons and the transport to another player instead: any Aurora68 Play
that is online, a Mac or the SE (Devices, below); the **Devices** list shows each with what it plays, and
**Radio** lists the internet radio stations to play on one.

Tune the SE to Live once (it is the second station in the stations file, or Radio > Open URL... with
`http://NAS:8423/live`; the server's bare address `http://NAS:8423/` works too, because a player that does
not ask for a web page is sent on to Live) and choose what it plays from your phone. Changes are heard within about
two seconds. While nothing is queued, or the channel is paused, it sends silence so the Mac stays
tuned; while nobody listens it holds its place, so a queue set before the Mac tunes in starts from its
first song. Song titles reach Aurora68 Play's window as with any station.

Anything on your network can drive it too: `POST /api/live` with JSON and the header `X-Aurora: 1`
(which a web page from elsewhere cannot send without asking, so no other site can change what plays):

| `action` | |
|---|---|
| `play`, `add`, `next` with `what` | replace the queue / add at the end / insert after the playing song; `what` is `s:ID` (a station: folder or artist, the ID from `/s/ID`), `f:FOLDER` or `t:FOLDER/SONG.m4a`; `"shuffle": 1` shuffles them |
| `skip`, `previous`, `pause`, `resume` | as on a player (`previous` restarts a song more than three seconds in) |
| `jump`, `remove` with `index` | play or take out the queue's song at that place (0 is the first) |
| `clear`, `shuffle`, `repeat` (`"on": true/false`) | empty the queue; shuffle the songs still to come; play the queue again when it ends |

`GET /api/now` says what is heard (the song, its time and length, the queue, the listeners).

```bash
curl -X POST -H 'X-Aurora: 1' -d '{"action":"skip"}' http://NAS:8423/api/live
```

## Getting albums: an indexer and SABnzbd
With an indexer that speaks the Newznab API (NZBGeek, by default) and SABnzbd, the panel gets a
**Get albums** button: search for an artist or an album, see each result's size, age and grabs, and
**Download** sends it to SABnzbd in its `music` category; a **Downloads** box shows SABnzbd's progress and
recent history for that category. Point SABnzbd's `music` category at your albums folder (SABnzbd's own
Config > Categories; its container must see that folder) and a finished album joins the library at the
next scan, within half a minute.

Set in docker-compose's `.env` (never in the compose file itself, and never in the repository):

| Variable | |
|---|---|
| `GET_INDEXER_KEY` | the indexer's API key (NZBGeek: My Account > API) |
| `GET_INDEXER_URL` | the indexer's address, `https://api.nzbgeek.info` by default |
| `GET_SAB_URL` | SABnzbd's address as this server reaches it, e.g. `http://192.168.1.20:7777` |
| `GET_SAB_KEY` | SABnzbd's API key (Config > General) |
| `GET_SAB_CATEGORY` | `music` by default |
| `GET_INDEXER_CATS` | Newznab categories to search, `3000` (audio: MP3, lossless ...) by default |

The keys stay on the server: it makes every call itself, a result's download link (which carries the
indexer's key) never reaches the browser (only a short id does), and neither key appears in the log or
in an error. Searching and downloading need the header `X-Aurora: 1`, so no other web site can spend
your indexer's searches. Off unless all three of `GET_INDEXER_KEY`, `GET_SAB_URL` and `GET_SAB_KEY` are set.

[Lidarr](https://lidarr.audio) does this and more (watching artists for new albums, upgrades, renaming
and tagging on import); its imports land in the music folder like anything else.

## IPv6 and IPv4
The server answers on both (`MUSIC_LISTEN=both`, the default; `ipv4` or `ipv6` for one only). Which
address it hands the Mac, in the stations file and on the panel, is `MUSIC_PREFER`:

- `asked` (the default): the address the browser or the Mac used to reach it;
- `ipv6`: the IPv6 address the request came to, if it came by one (it is known to reach the server);
  else this computer's IPv6 address (a stable one: not a temporary "privacy" address; a unique local
  `fd..` address first, then a global one, the documentation range `2001:db8::/32` last), or its IPv4
  address if it has no IPv6;
- `ipv4`: its IPv4 address, or IPv6 if it has no IPv4.

The address is written as numbers, so the Mac needs no name lookup. The log's second line lists the
addresses found; `MUSIC_IPV6_ADDRESS` and `MUSIC_IPV4_ADDRESS` choose one by hand. A single stations
file can be asked for either way: `/stations.txt?prefer=ipv6` or `?prefer=ipv4` (the panel links both).

With Docker, IPv6 needs nothing more than host networking (`network_mode: host`, as in
`docker-compose.yml`): the container answers on the computer's own addresses. Docker's bridge networks
have no IPv6 unless the Docker daemon is set up for it.

## On a NAS (Docker)
`docker-compose.yml` in this folder is a complete "project" (Synology Container Manager), "application"
(QNAP Container Station) or "stack" (Portainer, Unraid, TrueNAS). Change two things in it:

- **the music folder**, the left half of `/volume1/music:/music:ro`: Synology `/volume1/...`, QNAP
  `/share/...`, Unraid `/mnt/user/...`;
- **`PUID`/`PGID`**, a user and group that can read that folder. On Synology `id YOURUSER` in an SSH
  session says them (often `1026:100`); set them in the project's environment or write the numbers into
  `user:`.

Then start it (`docker compose up -d` from an SSH session, or the NAS's Create/Deploy button). It pulls
`ghcr.io/farsonic/aurora68-music-server`, which GitHub builds for Intel/AMD, 64-bit ARM and 32-bit ARM
NAS boxes from this folder whenever it changes. To build it on the NAS instead, comment out `image:` and
uncomment `build: .` with this folder copied there.

It uses **host networking**: the container answers on the NAS's own addresses, IPv4 and IPv6, on port
8000. Docker's usual bridge network has no IPv6 unless the Docker daemon is set up for it, so host
networking is the simple way to get both. If port 8000 is taken, set `MUSIC_PORT`.

`docker logs aurora68-music` shows what it is doing: the library scan (how many songs of each format,
and any folder it cannot read, with the user it runs as), and each listener and song. "0 songs" with a
"cannot read" line means `PUID`/`PGID` cannot read the music; with no such line, the wrong folder is
mounted.

Without Docker, on any computer with Python 3 (tested with 3.12 and 3.13):
```bash
python3 aurora68_music_server.py /path/to/music --name "NAS Music"
```

## Listening on the SE
- **One station:** Radio > Open URL... (Cmd-U) and type the address from the web page, e.g.
  `http://192.168.1.20:8000/s/1a2b3c4d`. Each folder's page shows its station's address.
- **The whole library in the Radio menu:** fetch `http://NAS:8000/stations.txt` and put it on the Mac as
  "Aurora68 Play Stations" in the Preferences folder (System 6: the System Folder), or add its lines to
  the one that is there. It makes a submenu named after the library, with "Everything, shuffled", each
  top-level folder as a station, and a submenu for each top-level folder with "All of ..., shuffled"
  and each folder inside it. Deeper folders play inside their parents' stations. A folder of albums
  named "Artist - Album" side by side is offered by artist instead: one station per artist playing all
  their albums, filed as record shops do ("The Cure" under C). A list longer than a menu on the SE's
  screen (18 items) is cut into alphabetical submenus ("#–C", "D–H", ...). A library that is one folder
  ("Albums") is shown as its contents. Each album still has its own station on the web page. It stays within
  Aurora68 Play's limits (128 stations and 48 submenus, 40 of the stations its own), and says so in a
  note if your library has more folders than that.
- If the Mac cannot look up the NAS's name (a `.local` name, say), ask for the file with the address the
  Mac should use: `http://NAS:8000/stations.txt?host=192.168.1.20` (an IPv6 address works too:
  `?host=fd00::20`).

Folder and song names are converted to the Mac's character set; anything it lacks becomes a question
mark.

## Aurora68 Play's Library
Aurora68 Play 1.2 browses the server itself: **Library > Show Library** (Command-L) lists the artists, an
artist's albums with their covers, an album's tracks under its cover, and plays a track, an album or an
artist with a queue (Previous, Next, Shuffle), the cover and the track in its player window. It finds the
server by itself: a UDP probe to port **8070**, sent as a broadcast and to the multicast group
**239.255.68.70** (Riptide's MacTCP interface sends the second where it refuses the first). The answer
carries the server's IPv4 and IPv6 addresses, and Play takes the one its **Connect By** preference names
(IPv6 First or IPv4 First, in the Library and Radio menus), the other if that fails. With host networking
(the compose file's way) the probe reaches the container; behind Docker's bridge network, or on another
subnet, choose **Library > Connect to Server...** and type the address, such as `192.168.1.20:8423`.
`MUSIC_DISCOVERY_PORT` moves the UDP port (0: no probe answered).

The Library's own requests are made for a 68k program written in C: text in Mac Roman, one record a
line ended by a Return, fields separated by tabs, a first line `A68M 1 KIND TOTAL FROM COUNT`, and
`?from=N&count=M` for a page of a long list (Play keeps a page, never the whole library). Covers come
as bitmaps dithered here, so the Mac only copies bits to the screen:

| | |
|---|---|
| `/mac/info` | the name, the version, how many artists, albums and tracks |
| `/mac/artists` | id, name, albums, tracks: sorted as record shops file them ("The Cure" under C) |
| `/mac/albums?artist=ID` | id, title, artist, tracks, seconds (no artist: every album) |
| `/mac/tracks?album=ID` or `?artist=ID` | id, number, title, artist, seconds, album id (an album's first listing reads its tags and lengths with ffprobe, then keeps them) |
| `/mac/search?q=WORDS` | artists (`A`), albums (`L`) and tracks (`T`) whose names hold every word (the words in Mac Roman) |
| `/mac/art?album=ID&w=64&h=64&depth=1` | the cover: `A68ART 1 W H DEPTH ROWBYTES`, then the rows. Depth 1: Bill Atkinson's dithering, 1 black. Depth 8: indices into the Mac's standard 256-colour palette ('clut' 8) |
| `/mac/arts?albums=ID,ID,...&w=32&h=32` | covers for a list of albums: `A68ARTS 1 N W H DEPTH ROWBYTES`, then for each a byte (1 a cover, 0 none) and its rows |
| `/t/ID?start=S` | one track as MP3 (an MP3 as it is, anything else converted), from S seconds, ending where the track does |

An album is a folder that holds songs itself; its artist is the part before " - " of a folder named
"Artist - Album", else the folder it is in. A cover is `cover.jpg`, `folder.jpg`, `front.jpg` and the
like beside the songs, else the picture inside the first song.

## Devices: every Aurora68 Play, and "play this there"
Since 22 November 2026 every Aurora68 Play (the macOS app on any Mac, the classic Play on the SE) tells
the server it is there, what it plays, and takes commands, like Spotify Connect: from the web panel's
**Play on** menu, or from another Play, an album, a station or a radio station can be sent to any player
that is online. The Live channel is listed as the device `live`. No player has to accept connections:
each fetches its commands by a long poll (the server holds the request up to 20 seconds and answers as
soon as a command comes), so a Mac behind its own firewall and a classic Mac with MacTCP work alike.
This section is the interface both Plays were written against; change it here first.

JSON for the web panel and the macOS Play; text for the classic Play, as the rest of `/mac/` is (Mac
Roman, lines ended by a Return, fields separated by tabs; no `A68M` first line). Everything that changes
something (announce, state, command, fetching commands) needs the header `X-Aurora: 1`; the lists do not.

| Request | Answer |
|---|---|
| `POST /api/devices/announce` `{"id", "name", "kind": "macos"\|"classic"\|"browser", "version"}` | `{"ok": true, "every": 20, "library": G}`. Send it every `every` seconds: a device not heard from for 60 is offline. The id: 1 to 64 letters, digits, `-` and `_`, made once and kept in the player's preferences (not `live`); the name: the Mac's own |
| `GET /mac/announce?id=ID&name=NAME&kind=classic&v=1.3` | `ok`, `20`, `G` (three lines); the name in Mac Roman, `%XX` for each byte |
| `POST /api/devices/ID/state` `{"playing", "title", "artist", "album", "folder", "position", "length", "volume": 0-100}` | `{"ok": true}`; on every change and with each announce (the macOS Play also every 5 seconds while playing). 404 before the first announce |
| `GET /mac/state?id=ID&p=1&pos=SECONDS&len=SECONDS&vol=N&t=TRACKID` | `ok`; the title, artist, album and length come from the catalogue by the track's id (`/mac/tracks`); `&title=...&artist=...` name a radio station instead |
| `GET /api/devices` | `{"devices": [{"id", "name", "kind", "version", "online", "last_seen", "state": {..., "at"}, "is_live"}, ...]}`: `live` first (its now-playing as its state, `listeners` how many), then the players by name, offline ones too (until 64 are known) |
| `GET /mac/devices` | one line a device: `id`, `name`, `kind`, `online` (1 or 0), `title`, `artist` |
| `POST /api/devices/ID/command` `{"action", "what", "shuffle": 1, "position", "volume"}` | `{"ok": true, "id": N}`, or `{"error"}` in words: 400 (not understood, nothing to play), 404 (no such device), 409 (not online). For `live` it is the Live channel's own action (`stop` empties its queue; `seek`, `volume` and radio are refused) and the answer carries `"live"`, what `/api/now` says |
| `GET /api/devices/ID/commands?wait=20` | `{"commands": [...]}`, waiting up to `wait` seconds (20 at most) for one; an empty list when the wait is over. Asking counts as being heard from |
| `GET /mac/commands?id=ID&wait=20` | for each command a line `N`, `action`, `position`, `volume`, then a line `T`, `tid`, `title`, `artist`, `seconds` for each of its tracks and `R`, `name`, `url` for a radio station; an empty answer when the wait is over |

Actions, as `/api/live`'s: `play` (replace what plays), `add` (at the end), `next` (after the playing
song) take `what`: `s:ID` (a station: folder or artist), `f:FOLDER` (`f:` alone is the whole library),
`t:FOLDER/SONG.flac`, or `r:N` (internet radio station N of `/api/radio`, with `play` only); `"shuffle": 1`
shuffles the songs. `pause`, `resume`, `skip` (the next song), `previous`, `stop`, `seek` with `position`
(seconds) and `volume` with `volume` (0 to 100) need nothing else. The server turns `what` into the songs
themselves, so the device gets them ready to play:

```json
{"id": 7, "action": "play", "shuffle": 1,
 "tracks": [{"path": "Gamma - Converted/01. Gamma - Closing Time.m4a", "tid": "k3c9d0qa",
             "title": "Closing Time", "artist": "Gamma", "album": "Converted", "seconds": 273}],
 "radio": {"name": "Groove Salad", "url": "http://ice2.somafm.com/groovesalad-128-mp3"},
 "position": 0, "volume": 80}
```

`tracks` (only for play, add and next; at most 2,000, a whole library shuffled is cut there; the first 40
with their tags and lengths read) or `radio` (a radio station: `tracks` is empty), `position` and `volume`
only where they were given. A modern player plays `/file/PATH` (the original file, with Range); the
classic Play plays `/t/TID` (MP3, converted where needed) or the radio station's address. Commands wait
for their device (32 at most; the oldest are dropped) until it asks.

```bash
curl -X POST -H 'X-Aurora: 1' -d '{"action":"play","what":"r:0"}' http://NAS:8423/api/devices/mac-0123456789ab/command
```

## Internet radio for every player
`GET /api/radio` gives `{"stations": [{"group", "name", "url"}, ...]}` and `GET /mac/radio` the same as
lines `group`, `name`, `url`: the classic Aurora68 Play's own list (`mac/piplay/stations.c`: SomaFM, the
listener-supported stations, stations by country; groups like `By country/Austria` are submenus), so
every player offers the same stations; the test checks that the server's copy and the classic Play's
agree. Your own stations come after them: put a stations file in the music folder, named
`Aurora68 Play Stations` (the classic Play's own file will do, Mac Roman or UTF-8) or `radio.txt`, or
name it with `MUSIC_RADIO=PATH`; it is read again when it changes. Its format is the classic Play's:

```
[Home]
Kitchen = http://192.168.1.20:8000/kitchen.mp3
```

The Live channel plays the library only: a radio station plays on a player.

## New albums without a restart
The folder is scanned again when a request comes and the last scan is more than 30 seconds old
(`MUSIC_RESCAN`); a scan that finds anything changed (a folder or a file added, renamed or removed)
adds one to the library's **generation**. `GET /api/info` gives it, with the name, the version and the
counts: `{"name", "version", "library": G, "artists", "albums", "tracks", "every": 20}`; the announce
answer carries it too (and `/mac/info`'s sixth field). Players remember it and fetch their lists again
when it changes: the macOS Play does within 20 seconds of a scan, so an album copied in shows up within
about a minute, without restarting anything.

## Addresses
| | |
|---|---|
| `/` | the control panel |
| `/live` | the Live channel: what the panel plays |
| `/api/now`, `/api/live`, `/api/addresses` | what Live plays; changing it (POST, above); the addresses handed out |
| `/api/catalog`, `/api/tracks?album=ID` (or `?artist=ID`) | JSON for the macOS Aurora68 Play (`macos/Aurora68Play`): every artist and album; an album's tracks with titles, lengths and the files to play through `/file/` |
| `/art?dir=FOLDER` | a folder's cover picture (`cover.jpg`, `folder.jpg`, ... or the one inside its first song) |
| `/browse/FOLDER/` | a plain page of a folder |
| `/s/ID` | a folder's station by its short name (the stations file uses these: Aurora68 Play's addresses are at most 160 bytes) |
| `/radio/FOLDER/` | the same station by the folder's path; `/radio/` is the whole library |
| `?shuffle=1`, `?loop=0` | on a station: random order; one pass, then the stream ends (stations loop by default) |
| `/file/FOLDER/NAME.mp3` | one file as it is, with Range for players that seek |
| `/playlist.m3u?dir=FOLDER` | an M3U of a folder's files |
| `/stations.txt` | the library as an Aurora68 Play stations file; `?host=ADDRESS` for the address written in it |
| `/api/list?dir=FOLDER` | JSON: the folder's station, folders and files |
| `/mac/...`, `/t/ID` | Aurora68 Play's Library (above) |
| `/api/info` | the library's generation and counts (above) |
| `/api/devices`, `/api/devices/...`, `/mac/announce`, `/mac/state`, `/mac/devices`, `/mac/commands` | the players (Devices, above) |
| `/api/radio`, `/mac/radio` | the internet radio stations (above) |

## Settings
| Option | Environment | |
|---|---|---|
| folder | `MUSIC_DIR` | the music (`/music` in the container) |
| `--port N` | `MUSIC_PORT` | 8000 |
| `--name NAME` | `MUSIC_NAME` | the library's name in Aurora68 Play's menu and on the page ("Music") |
| `--listen both\|ipv4\|ipv6` | `MUSIC_LISTEN` | answer on IPv6 and IPv4 (the default), or one of them only |
| `--prefer asked\|ipv6\|ipv4` | `MUSIC_PREFER` | the address handed to the Mac (above) |
| | `MUSIC_IPV6_ADDRESS`, `MUSIC_IPV4_ADDRESS` | that address by hand |
| `--bind ADDR` | `MUSIC_BIND` | one address to listen on only |
| `--max-listeners N` | `MUSIC_MAX_LISTENERS` | 16 at once |
| | `MUSIC_COPIES` | `hide` (the default): copies of one track in an album folder (the same number and title, sizes within 2%, as a download made twice leaves) are listed once, the best kept (FLAC, M4A, MP3, then the larger); `show` lists every file |
| `--bitrate N` | `MUSIC_BITRATE` | kbit/s of the MP3 other formats become: 192 (128 is plenty for the SE's speaker; up to 320) |
| `--ffmpeg PATH` | `MUSIC_FFMPEG` | ffmpeg, found on the PATH; `""` plays MP3s only |
| | `MUSIC_RESCAN` | seconds a scan of the folder is trusted: 30 |
| | `MUSIC_RADIO` | your own stations file (else `Aurora68 Play Stations` or `radio.txt` in the music folder) |
| | `MUSIC_DEVICE_TTL` | seconds after which a player not heard from is offline: 60 |

New music is found by itself: the folder is scanned again when a request comes and the last scan is
more than 30 seconds old, and players are told (New albums without a restart, above).

## How a station is sent
An MP3's frames are found and sent as they are (no conversion, so no quality is lost and the NAS does
almost no work). Any other format goes through ffmpeg into MP3 at 44.1 kHz stereo as it is sent (its
cover picture left out), a few per cent of one core per listener; when the listener leaves, that ffmpeg
stops. The ID3 tags at the front, the ID3v1 and APE tags at the end and the encoder's
Xing/Info frame are left out, because inside a stream they are noise to a decoder. Files at any bit rate
or sample rate, constant or variable, follow each other: Aurora68 Play's decoder takes each frame's rate
from the frame. The first six seconds go at once, so a player's buffer fills quickly; after that frames
go at the rate they play, timed by the frames' own headers. Song titles go as Icecast-style metadata
every 16,000 bytes to players that ask for them, from the ID3 tag ("Artist - Title") or the file's name.
A file that holds no MP3 audio is passed over, once, with a line in the log.

## Test
```bash
python3 tools/music-server/test_server.py
```
It needs ffmpeg and a C compiler. It makes a small library (folders in folders, 44.1 and 48 kHz, VBR,
22 kHz mono, ID3v2.3 and 2.4, M4A with AAC and a cover picture, ALAC, FLAC, "Artist - Album" folders,
accented names, a broken file, links that lead out of the folder) and checks
the server against Aurora68 Play's own parsers, built on the host (`play_check.c`, with `mac/piplay/icy.c`
and `stations.c`). It checks the stations file and its limits, the titles in order and in Mac Roman, and
that the audio decodes without an error at its full length. It also checks the pacing, Range, M3U, JSON,
the page, that every path out of the folder is refused, and IPv6 over `::1` where the computer has it
(where it has not, as in the build environment this was written in, it checks that the server says so and
serves IPv4). On a server of its own (a rescan every second, players offline after three seconds) it
checks the devices: announce, state and the list in JSON and in the classic Play's text, commands queued
and fetched by the long poll (at once, and when one comes during the wait), radio stations by index, the
Live channel as a device, refusals, a player going offline and coming back; the radio list against
`mac/piplay/stations.c` and a stations file of your own; and the library's generation going up when an
album is copied in, with the catalogue listing it, and not otherwise.
GitHub runs it before each image build.

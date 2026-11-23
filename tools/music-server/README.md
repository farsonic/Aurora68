# The Aurora68 music server

A folder of music, organised in folders however you like, served as internet radio stations over IPv4
and IPv6. MP3s are sent as they are; FLAC, M4A (AAC and ALAC), Ogg, Opus, WAV and AIFF are converted to
MP3 as they are sent (ffmpeg, which the Docker image has; 192 kbit/s by default), and music videos beside the
albums are listed and played too (Music videos, below). Every folder is a
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

Its look (22 November 2026) is the Setup and Recovery page's (`tools/recovery/page.html`), so the project's two web
pages match, as the owner asked ("an 80s Apple style and colour"): the Macintosh of 1984-89, black on white over the
grey desktop pattern, each box a window with a pinstriped title bar and a close box, a hard drop shadow, rounded
buttons with the default button's ring, messages in an alert box, Chicago and Monaco where the computer has them
(nothing is fetched from the network), the six colours of the period in a stripe under the top bar and one for each
window, and the Aurora68 logo (`docs/img/aurora68-logo.svg`, inline, and the favicon). On a phone the windows stack,
Now Playing first.

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
| `/mac/tracks?album=ID` or `?artist=ID` | id, number, title, artist, seconds, album id, each album in disc and track order (an album's first listing reads its tags and lengths with ffprobe, then keeps them) |
| `/mac/search?q=WORDS` | artists (`A`), albums (`L`) and tracks (`T`) whose names hold every word (the words in Mac Roman) |
| `/mac/art?album=ID&w=64&h=64&depth=1` | the cover: `A68ART 1 W H DEPTH ROWBYTES`, then the rows. Depth 1: Bill Atkinson's dithering, 1 black. Depth 8: indices into the Mac's standard 256-colour palette ('clut' 8) |
| `/mac/arts?albums=ID,ID,...&w=32&h=32` | covers for a list of albums: `A68ARTS 1 N W H DEPTH ROWBYTES`, then for each a byte (1 a cover, 0 none) and its rows |
| `/t/ID?start=S` | one track as MP3 (an MP3 as it is, anything else converted), from S seconds, ending where the track does |

**The same interface for the macOS Play** (22 November 2026: both Plays share one C core, `play/core`, and one
wire format, this one; docs/08 item 107). Two parameters, on any `/mac/` request, which the classic Play does
not send, so its answers are unchanged:

| | |
|---|---|
| `enc=utf8` | the answer in UTF-8 (`Content-Type: text/plain; charset=utf-8`) and the request's words (`q=`, `name=`, `title=`, `what=`) read as UTF-8; without it Mac Roman both ways |
| `full=1` | the longer records: `/mac/info` adds `every` (seconds between announces); `/mac/albums` adds the artist's id and the album's folder; `/mac/tracks` adds the path (for `/file/`), the album's title and its folder; `/mac/devices` adds `playing`, `position`, `length`, `volume`, `album`, `folder`, `at`, `last_seen`, `is_live`, `version`, `listeners` (what `/api/devices` has); `/mac/commands` adds `shuffle` to a command's line and the path and album to each `T` line. Paths, folders and addresses are never cut short (other fields stop at 120 characters) |
| `/mac/now` | the Live channel, as `/api/now`: `A68M 1 now QUEUE 0 COUNT`, then `N` name, playing, paused, repeat, pos, changes, listeners; `C` path, folder, title, elapsed, length (the song heard, if any); a `Q` path, title for each queued song; an `L` who, since for each listener |
| `/mac/command?to=ID&action=A` | "play this there", as `POST /api/devices/ID/command`, with `what`, `shuffle=1`, `position` (seconds, a fraction allowed), `volume`; to `live` also the Live queue's own actions (`jump` and `remove` with `index`, `clear`, `shuffle`, `repeat` with `on=1` or `0`). Needs `X-Aurora: 1`. Answers `ok` and the command's number, or the refusal in words (400, 404, 409) |

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

JSON for the web panel; text for both Plays, as the rest of `/mac/` is (Mac Roman, or UTF-8 with `enc=utf8`;
lines ended by a Return, fields separated by tabs; no `A68M` first line). The macOS Play used the JSON until 22
November 2026 and the text since (`enc=utf8&full=1`, above); the JSON is unchanged. Everything that changes
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

## Music videos
Since 22 November 2026 the server also finds the music videos in the library: `.mp4`, `.m4v`, `.mov`, `.mkv`,
`.webm`, `.avi`, `.mpg`, `.mpeg` and `.ts` files (any case), in the same folders as the albums or in an artist's
folder. Nothing has to be moved or renamed. Each video is attached:

- in an album's folder (one holding songs): to that album and its artist;
- in a folder named `Videos`, `Video`, `Music Videos` or `Clips` inside an album's or an artist's folder: as if it
  were in that folder;
- in an artist's folder (one whose albums are folders inside it), or a folder inside that: to that artist;
- in any other folder: to the artist its name gives ("Artist - Videos" gives "Artist", else the folder's name);
- at the top of the library: to the artist its own name gives ("Artist - Title.mp4"), else the library's name.

A video's title is its file's name tidied as a track's is ("03. Artist - Title (Official Video).mp4" is
"Title (Official Video)", number 3). Its length, picture size and codecs come from ffprobe, asked once per file and
kept in the cache folder (`videos.json`, by path, modification time and size), so a rescan of a slow NAS costs
nothing and a restart probes nothing it has seen; a file ffprobe finds no picture in is passed over with a line in
the log. Copies of one video (the same title, sizes within 2%, as a download made twice leaves) are listed once, the
one that plays as it is kept (MUSIC_COPIES as for tracks). Videos count in the library's generation (a video copied
in makes the players fetch their lists again) and in `/api/info` (`"videos"`).

**How a video is sent**, the cheapest way that plays on a Mac, decided per file from what ffprobe said:

| | when | what is sent |
|---|---|---|
| as it is | MP4, M4V or QuickTime holding H.264 (8-bit 4:2:0) or HEVC (tagged `hvc1`), with AAC, ALAC or MP3 sound or none | `/video/ID`: the file, with Range, as AVFoundation (the macOS Play, Safari, QuickTime Player) and browsers ask for it |
| remuxed | H.264 or HEVC in another container (MKV, TS, AVI), or with sound a Mac lacks (AC-3, DTS, Opus, Vorbis), or HEVC tagged `hev1` | the picture copied, not encoded again, into fragmented MP4 as it is sent; sound a Mac lacks converted to AAC (sound is cheap; the picture is what a NAS cannot afford) |
| transcoded | anything else: VP9, AV1, VP8, MPEG-2, MPEG-4 Part 2, 10-bit or 4:2:2 H.264 | H.264 (x264 `veryfast`, CRF 21 under `VIDEO_BITRATE`, no taller than `VIDEO_MAX_HEIGHT`, a keyframe every 2 seconds) and AAC at 192 kbit/s; at most `VIDEO_TRANSCODES` at once (503 beyond, "N transcodes running already"); refused with `VIDEO_TRANSCODE=off` (415, in words). The reason is logged |

A remuxed or transcoded stream has no length before it is made, and AVFoundation does not play a progressive file
whose server cannot answer a byte range (its error -11850, "server not correctly configured"). So those two are also
offered as HLS, which AVFoundation plays as it grows: `/video/ID/index.m3u8`, segments of fragmented MP4 of four
seconds written to the cache folder as ffmpeg makes them (`/video/ID/init.mp4`, `/video/ID/seg00000.m4s`...), the
playlist served with `#EXT-X-START:TIME-OFFSET=0` so a player starts at the beginning, not at the growing end. A
finished conversion is kept (until the cache's limit) and served again without ffmpeg; one nobody has fetched from
for a minute is stopped. The macOS Play opens the file for a video sent as it is and the HLS for the others; the
panel does the same in Safari and takes the plain fragmented MP4 (`/video/ID`, `?start=S` to begin later) in other
browsers.

Also for each video:

| | |
|---|---|
| `/vthumb/ID` | its poster: a JPEG of the picture a tenth of the way in (past the titles), 480 pixels wide at most, kept |
| `/vaudio/ID?start=S` | its sound as MP3 at the library's bit rate, an MP3 stream with an end exactly as `/t/ID` is: the classic Play plays a video so. Converted as it is sent the first time and kept whole (when sent from the start), then sent from the cache. `/t/ID` with a video's id gives the same (an older Play given a video as a track) |
| `/vmpeg/ID?w=W&h=H&start=S&rate=R` | for the Aurora68 Video Decoder card (the kernel's MPEG-1 card, docs/16): an MPEG program stream made exactly as the video helper's `/play` makes it (`tools/video-helper`, the same ffmpeg arguments): MPEG-1 video fitted into W x H and padded black (even sizes, 64 to 1280 x 720, 320 x 240 when not given), 25 frames a second, no B-frames, a keyframe every second, about 0.09 bit per pixel per frame (300 to 2,500 kbit/s), or `rate=R` kbit/s of video (150 to 2,500) when a client asks (the classic Play 1.6 does: 400 kbit/s at 320 x 240, for the SE's network; 23 November 2026); MP2 sound at 44.1 kHz stereo, 128 kbit/s. No Content-Length; it ends with the video, and stops (with its ffmpeg) when the client closes. Always a transcode: counted against `VIDEO_TRANSCODES`, refused with `VIDEO_TRANSCODE=off`. `mpeg1` in the capabilities says a server has it. The classic Aurora68 Play 1.6 uses it with the video decoder card |
| `/file/PATH` | the original file, with Range, as for songs |

**JSON:** `GET /api/videos` (every video), `?artist=ID` or `?album=ID`: `{"videos": [{"id", "title", "artist",
"artist_id", "album" (its id, or empty), "album_title", "folder", "path", "number", "seconds", "width", "height",
"vcodec", "acodec", "how": "direct"|"remux"|"transcode", "why" (in words), "playable", "video": "/video/ID", "hls"
(empty for one sent as it is), "poster": "/vthumb/ID", "audio": "/vaudio/ID"}]}`. `/api/tracks` answers carry the
album's (or the artist's) `"videos"` after `"tracks"`; `/api/catalog` counts each album's and artist's `"videos"`
and lists artists with videos and no album; `/api/list` (the panel's) has a folder's `"videos"` (a folder of videos
alone shows in its parent) and each grouped artist's; `/api/info` has `"videos"`, `"capabilities"` (`videos`,
`remux`, `hls`, `transcode`, `mpeg1`, as this server is set up) and `"transcoding"` (how many run now).

**The players' text (`/mac/`).** A third parameter, `videos=1`, which the classic Play 1.4 and the macOS Play 1.2 do
not send: without it every answer is byte for byte what it was (the test compares every `/mac/` answer with those
of a server with `VIDEOS=off`). With it, in Mac Roman or with `enc=utf8`, short or with `full=1`:

| | with `videos=1` |
|---|---|
| `/mac/info` | adds `every`, the number of videos and the capabilities: `name, version, artists, albums, tracks, generation, every, videos, caps` (`caps` like `videos,remux,hls,transcode,mpeg1`) |
| `/mac/artists` | a fifth field, the artist's videos; an artist with videos and no album is listed (0 albums) |
| `/mac/tracks?album=ID` or `?artist=ID` | the tracks, then the album's (or artist's) videos as `V` records: `V, id, title, artist, seconds, album (its id, or empty), width, height`; with `full=1` also `path` (for `/file/`), the album's title, the folder and how it is sent (`direct`, `remux`, `transcode`). TOTAL counts both and pages run through the tracks, then the videos. An artist with videos alone has an answer here (without `videos=1`, 404 as before) |
| `/mac/videos` | (new) every video, or `?artist=ID` or `?album=ID`: `A68M 1 videos TOTAL FROM COUNT`, then `V` records as above |
| `/mac/search` | also `V, id, title, artist, album, seconds` (after the albums, before the tracks), and artists with videos alone |
| `/mac/devices` | the full record (as `full=1`) and an 18th field: `1` if the device shows videos |
| `/mac/commands` | a video in a command is a `V` line: `V, id, title, artist, seconds`, with `full=1` also `path, album, how, width, height`. Without `videos=1` it is a `T` line whose id `/t/` answers with the sound |

**Devices.** A player says whether it shows videos: `"video": true` in `POST /api/devices/announce`, `&video=1` on
`/mac/announce` (the macOS Play 1.3 does; the classic Play does not: it plays their sound until the video card's
program exists). `/api/devices` has `"video"` for each. `what` takes `v:ID`, a music video, with `play`, `add` and
`next`, to any device: the command's track says `"video": true`, `"how"`, `"width"`, `"height"`; to a device that
shows none the answer adds `"audio_only": true` and the device plays its sound; to Live, its sound joins the queue. A
device reporting a video's id in `/mac/state` (`t=ID`) is named from the catalogue, its state with `"video": true`.

**The control panel** lists a folder's videos under its songs (an artist's under their albums), and a Videos
button (shown when the library has any) lists them all, each as its poster with its title and length. A click opens
it in a window of the page (HTML5 `<video>`: the file, HLS in Safari, else the fragmented MP4), with **Play on** the
chosen player (Live: its sound) and **Add to queue**.

### Videos linked to songs
Since 23 November 2026 (docs/08 item 115) a video also belongs to the songs it is of, wherever it sits: "Daft Punk -
One More Time (Official Video).mp4" in the artist's folder is the video of "One More Time" on *Discovery*. The owner
asked for it to find videos from the songs, and to play a song's video in its place ("link them to a song to
alternate"). A video is a song's when the two have

- **the same artist**: the one the video is attached to (above), or the one in front of its title ("Daft Punk -
  Around the World.mp4" among anybody's videos); compared by the first name only, case, "The" and punctuation aside
  ("Daft Punk, Julian Casablancas" is Daft Punk);
- **the same title**, both from the files' names (the track number and an "Artist - " in front gone, as for tracks),
  Unicode NFKC and casefolded, punctuation as spaces, and the tags in brackets or after " - " left out when they only
  say what kind of file it is: `(Official Video)`, `(Official Music Video)`, `(Music Video)`, `(Lyric Video)`,
  `(Lyrics)`, `(Visualizer)`, `(Audio)`, `[HD]`, `[4K]`, `(1080p)`, `(Remastered 2011)`, `- 2009 Digital Remaster`,
  `(Explicit)`, `(Clean)`, `(M/V)` and their mixtures, and the featured artists (`feat.`, `ft.`, `featuring`, `with`,
  in brackets or not). Every other tag names a version and is kept: `(Live)`, `(Remix)`, `(Radio Edit)`, `(Acoustic
  Version)`.

Three tries, the first that finds a song wins: the whole titles (a "Live" video is the "Live" song's), then the
video's plain title against a song's whole one (a Live video with no Live song is the studio song's), then plain
against plain ("One More Time (Official Video)" is "One More Time (Radio Edit)"'s only when nothing better was
found). One video may be of several songs (the same song on an album and its deluxe edition): it is linked to all of
them; a song has one video, its best (the earliest try, then a video of its own album, then of its artist, then the
first by path). Tags are not read for this: the links are made with the catalogue at each scan from the names alone,
cost no ffprobe, and are the same whatever has been listed before.

What changes in the answers:

| | |
|---|---|
| `/api/tracks` | each song with a video has `"video": ID`; each video (also in `/api/videos`) has `"tracks": [IDs]`, its songs |
| an album's videos | `/mac/tracks?album=ID&videos=1`'s `V` records, `/mac/videos?album=ID`, `/api/tracks` and `/api/videos?album=ID` list the videos of the album's songs as well as those in its folder, each once, in the order of its songs; `/api/catalog` and `/mac/albums` count them |
| an artist's videos | those attached to it and those of its songs, each once (`/mac/artists`' fifth field counts them) |
| `/mac/tracks` with `full=1&videos=1` | a tenth field after the folder: the song's video's id, empty when it has none. Only with both (the macOS Play 1.4 sends both; the classic Play sends neither, and its `T` records are what they were) |
| `/mac/albums` with `full=1&videos=1` | an eighth field: the videos the album lists |
| `/api/list` | `"file_videos"`: `{file name: video}` for the folder's songs that have one; the panel marks them with a film frame that opens the video as its poster does |

An album's songs are always listed in disc and track order (23 November 2026): from the names ("01. Title") and the
tags (track and disc numbers, read with an album's first listing), before the answer is cut into pages. Until then
files named by title alone ("Aerodynamic.flac", as some downloaders name them) came in title order: the sort of a
page by number asked the list being sorted whether any row had a number, and to Python a list being sorted is empty
(docs/08 item 115).

**Settings** (in docker-compose's `environment:`):

| Environment | |
|---|---|
| `VIDEOS` | `off`: no videos found, listed or served (`on`, the default) |
| `VIDEO_TRANSCODE` | `off`: refuse what would need encoding again (a slow NAS); files sent as they are and remuxes still play. `on` by default |
| `VIDEO_TRANSCODES` | transcodes at once (HLS, streams and the video card's MPEG-1 together): 1. A NAS's CPU encodes about one 1080p H.264 at its own speed |
| `VIDEO_BITRATE` | kbit/s at most for a transcode's picture: 4000 |
| `VIDEO_MAX_HEIGHT` | a transcode taller than this is scaled down: 1080 |
| `VIDEO_PRESET` | x264's speed: `veryfast` (`ultrafast` ... `medium`) |
| `VIDEO_CACHE_MB` | posters, soundtracks and finished HLS conversions kept, at most: 2048 MB (the least recently used go first) |
| `MUSIC_CACHE` | the folder for all that and `videos.json`: the system's temporary folder by default (in the container `/tmp`, emptied by each image update), so give it a folder of its own the server's user can write |

```yaml
    volumes:
      - /volume1/music:/music:ro
      - /volume1/docker/aurora68-cache:/cache      # a folder PUID can write
    environment:
      MUSIC_CACHE: "/cache"
      # VIDEO_TRANSCODE: "off"
      # VIDEO_TRANSCODES: "1"
```

## Internet radio for every player
`GET /api/radio` gives `{"stations": [{"group", "name", "url"}, ...]}` and `GET /mac/radio` the same as
lines `group`, `name`, `url`: the classic Aurora68 Play's own list (`play/core/stations.c`: SomaFM, the
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
| `/api/videos`, `/mac/videos`, `/video/ID`, `/video/ID/index.m3u8`, `/vthumb/ID`, `/vaudio/ID`, `/vmpeg/ID` | music videos (above) |

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
| | `MUSIC_CACHE` | the server's own folder for what it makes once (video posters, soundtracks, HLS, `videos.json`): the system's temporary folder |
| | `VIDEOS`, `VIDEO_TRANSCODE`, `VIDEO_TRANSCODES`, `VIDEO_BITRATE`, `VIDEO_MAX_HEIGHT`, `VIDEO_PRESET`, `VIDEO_CACHE_MB` | music videos (above) |

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
the server against Aurora68 Play's own parsers, built on the host (`play_check.c`, with `play/core/icy.c`
and `stations.c`). It checks the stations file and its limits, the titles in order and in Mac Roman, and
that the audio decodes without an error at its full length. It also checks the pacing, Range, M3U, JSON,
the page, that every path out of the folder is refused, and IPv6 over `::1` where the computer has it
(where it has not, as in the build environment this was written in, it checks that the server says so and
serves IPv4). On a server of its own (a rescan every second, players offline after three seconds) it
checks the devices: announce, state and the list in JSON and in the classic Play's text, commands queued
and fetched by the long poll (at once, and when one comes during the wait), radio stations by index, the
Live channel as a device, refusals, a player going offline and coming back; the radio list against
`play/core/stations.c` and a stations file of your own; and the library's generation going up when an
album is copied in, with the catalogue listing it, and not otherwise; and the macOS Play's side of `/mac/`
(UTF-8, the full records, a folder longer than 120 characters whole, `/mac/now`, `/mac/command` to Live and to a
player, its refusals). Music videos (videos_check, 22 November 2026): ffmpeg makes four of a few seconds at 160 x 120
in a temporary folder (an MP4 of H.264 and AAC, its copy as QuickTime, an MKV of H.264 and AAC, a WebM of VP9 and
Opus in an artist's Videos folder) and a .mp4 with no picture; the test checks the scan, the copy left out, the
facts from ffprobe and their cache, the attachment, Range, the remux (ffprobe reads it, ffmpeg decodes it cleanly,
and its H.264 bytes are the original's) and its HLS, the transcode, a transcode refused with `VIDEO_TRANSCODE=off`
and beyond `VIDEO_TRANSCODES=1` (a minute-long video held open by a client that reads no more), no ffmpeg left
behind, posters, soundtracks through the classic Play's parser (from the start and from 2 s, then from the cache),
`/vmpeg/` at three sizes (MPEG-1 at the size asked, MP2 at 44.1 kHz stereo, no B-frames), the JSON, the `/mac/` text
with `videos=1` both ways, every `/mac/` answer without it byte for byte a `VIDEOS=off` server's, and videos sent to a
Mac, to the SE (its sound) and to Live (62 checks; 203 in all). The shared core's own test (`tools/play_core_fixtures.py`, in `make -C mac check`) starts
this server too and reads its answers with the core both Plays use.
GitHub runs it before each image build.

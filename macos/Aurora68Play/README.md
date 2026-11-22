# Aurora68 Play for macOS

The music server's player on a modern Mac (macOS 13 Ventura or later, Apple silicon or Intel). It is
the companion of the Mac SE's Aurora68 Play: the same library (`tools/music-server`), shown the way a
music app is shown today, and a "Play on" choice that sends the music to the SE through the server's
Live channel, or to any other Aurora68 Play on the network.

- **Library:** artists and albums as a grid of covers, an artist's albums, an album with its cover
  and songs (number, title, length). The search field in the toolbar (Command-F) filters artists and
  albums as you type.
- **Playing on this Mac:** the original files, as they are on the server: MP3, AAC and Apple
  Lossless (M4A) and FLAC, played by AVFoundation with real seeking and no gap between songs. Play,
  Shuffle, Add to Queue, Play Next (right-click a song or an album); Previous and Next, a position bar
  (click to jump, or drag), volume, shuffle and repeat (all, one) in the bar along the bottom.
- **New albums without a restart:** the server counts its library's changes; the app hears the count
  every 20 seconds and fetches the artists and albums again when it changes, keeping what is open.
- **Now Playing:** the song and its cover in Control Center; the keyboard's media keys work. The space
  bar plays and pauses, Command-Left and Command-Right are Previous and Next.
- **Play on Live (Mac SE):** the toolbar's "Play on" menu (or Option-Command-2). Play, the queue and
  the buttons then control the server's Live channel, which every listener hears at once: the SE's
  Aurora68 Play tuned to it (Library > Listen Live). The bar shows what Live plays, and how many
  listen. "Listen here" (the headphones) plays Live on this Mac as well.
- **Stations (Live and Radio, in the sidebar):** stations to start, on whatever "Play on" names, or
  anywhere else from a station's menu: Everything, shuffled; each artist (all their albums, shuffled);
  and the internet radio stations every Aurora68 Play offers (SomaFM, Radio Paradise, KEXP, stations by
  country, and your own: tools/music-server/README.md, "Internet radio"). Below them, Live's queue (play
  or remove a song) and its listeners, and the players on the network.
- **Other players ("Play on" another Mac, or the SE):** every Aurora68 Play tells the server it is there,
  this one included (by the Mac's computer name, System Settings > General > Sharing). "Play on" lists
  every one that is online; choose one and Play, the queue and the bar's buttons (pause, next, previous,
  seek, volume) go to it, and the bar shows what it plays. The same from the other side: another Mac, or
  the server's web panel, can play music on this one.
- **Finding the server:** on start it asks the network (the server answers the probe
  `AURORA68-MUSIC?` on UDP port 8070, broadcast or to the group 239.255.68.70) and connects to the
  first that answers. Or type its address in Settings (Command-,): `192.168.1.20:8423`,
  `[fd00::20]:8423`, `nas.local:8423`; the port is 8000 if none is given.
- **IPv6 or IPv4:** Settings > Connect by: IPv6 first or IPv4 first. A server's name is looked up and
  its addresses tried in that order, as numbers (the system's own choice would not honour the
  preference); the discovery answer carries both of the server's addresses and is ordered the same
  way. If the preferred one does not answer within four seconds, the other is tried.

## Getting it
GitHub builds it for every change: the repository's Actions tab, the workflow "macos player", the
latest green run, the artifact **Aurora68-Play-macOS** (a zip of `Aurora68 Play.app`). Unzip it and
drag the app to Applications.

### Opening it the first time
The app is signed "ad hoc", not with an Apple developer account, so macOS will not open it with a
double-click the first time ("cannot be opened because Apple cannot check it"). Either:
- System Settings > Privacy & Security, scroll down: "Aurora68 Play was blocked", **Open Anyway**; or
- in Terminal: `xattr -dr com.apple.quarantine "/Applications/Aurora68 Play.app"`

macOS then asks once whether Aurora68 Play may find devices on your local network: allow it, or it
cannot reach the server (System Settings > Privacy & Security > Local Network turns it on later).

## Building it yourself
On a Mac with Xcode or its command line tools (`xcode-select --install`):

```bash
cd macos/Aurora68Play && ./make-app.sh
```

`build/Aurora68 Play.app` is the result (universal, ad-hoc signed). At the repository's top, where
`Package.swift` is since 22 November 2026, `swift test` runs the tests and `swift run Aurora68Play` starts the
app without making the bundle (without its icon and Info.plist).

## One code base with the SE's Aurora68 Play (22 November 2026)
The owner asked for "a common code base between the two". Everything that is not interface is now one portable
C core, `play/core` (its README says what is in it), which this app and the classic Play on the SE both compile:
the server's protocol and its answers, the catalogue's records and pages, the play queue, addresses and the
discovery answer, the devices' rules, track names, a station's playlist, the stations list. Here it is the
Swift package's C target **PlayCore**; **Aurora68Core** is its thin Swift side (the types the views show, made
from what the core reads, and calls into the core for every rule). What stays Swift is what is the platform's:
SwiftUI, AVFoundation and MediaPlayer, URLSession and the BSD sockets of the probe, and the library filter's
folding of case and accents (Foundation's Unicode tables).

- **One wire format: the server's `/mac/` text**, the one the SE reads, instead of the JSON. This app asks with
  `enc=utf8` (UTF-8 both ways, so a name with a character Mac Roman lacks still arrives whole) and `full=1` (the
  longer records it needs: an album's folder, a track's path for `/file/`, a player's whole state); the classic
  Play sends neither and gets what it always got. The catalogue comes page by page (500 at a time, the server's
  most). A server from before 22 November 2026 answers without the folders: the app says to update it.
- **Why `Package.swift` is at the top:** a SwiftPM target must lie inside its package's folder, and the classic
  Play's Makefile reaches the core from `mac/`; so the package is the repository and its targets name their
  folders (`play/core`, `macos/Aurora68Play/Sources/...`). Nothing else in the repository is part of it.
  `make-app.sh` builds with `--package-path ../..`; the products are in `.build` at the top.
- **What changed for the owner:** after a probe, the address the answer came from is tried before the IPv4
  address the server names for itself (inside a container that may be the container's); songs added to a queue
  that has played to its end are the ones that play, where the old queue started again from its first song; a
  radio station whose address is a `.pls` or `.m3u` playlist plays its first stream; a server without radio
  stations still offers the built-in ones; the device's id, when one is made, is the core's (`mac-` and twelve
  letters and digits; a kept one stays). An artist's name is taken off a file name's front in ASCII letters of
  either case (the core has no Unicode tables), where it used to match "É" with "é" too.

## How it is made
- `Package.swift` (at the repository's top): **PlayCore** is the shared C core (`play/core`). **Aurora68Core**
  is its Swift side, tested by `Tests/Aurora68CoreTests`: `Core.swift` (the bridge: an answer held where the
  core takes it apart in place, C strings to Swift and back), `Models.swift` (the catalogue, tracks and Live
  from the server's text), `Devices.swift` (the players, their commands as what the player does, a Command's
  request, the radio list, the library's generation, the id, a click on the position bar as a time, and
  `DeviceSession` over the core's devices.c), `Address.swift` (addresses as typed, URLs with IPv6 in brackets,
  percent-encoded paths, the discovery answer, the preference), `TrackName.swift` (number and title from
  "01. Artist - Title.m4a"), `PlayQueue.swift` (the queue: the core's rules over Swift values), `MusicAPI.swift`
  (the HTTP calls, the playlist of a station). **Aurora68Play** is the app: `App.swift`
  (the app, menus, keys), `AppModel.swift` (connecting, the library, routing to This Mac or Live),
  `Player.swift` (AVQueuePlayer with the playing song and the next; Now Playing and media keys),
  `LiveController.swift` (the Live channel), `DeviceLink.swift` (this Mac as a player: the announce,
  its state, the commands by long poll; the list of the others), `Network.swift` (the discovery probe and
  name lookup, BSD sockets), `LibraryViews.swift` and `PlayerViews.swift` (SwiftUI; the position bar is
  `Scrubber`: SwiftUI's Slider on macOS does not report the end of a click on its bar, which is why a
  click never seeked before 22 November 2026).
- What it asks of the server (since the shared core, 22 November 2026; tools/music-server/README.md): the
  `/mac/` text with `enc=utf8&full=1`: `info`, `artists` and `albums` (every page), `tracks?album=ID` (or
  `?artist=ID`), `now`, `command?to=ID&action=...` (to Live or another player; header `X-Aurora: 1`, sent with
  every request), `radio`, `devices`, `announce`, `state`, `commands?wait=20`; and, not text, `GET /file/PATH`
  (with Range), `GET /art?dir=FOLDER`, `GET /live`. Before that it asked for the JSON (`/api/catalog`,
  `/api/tracks`, `/api/now`, `POST /api/live`, `/api/devices...`), which the server still serves.
- `make-app.sh` builds for arm64 and x86_64, joins them with `lipo`, makes the icon (`make-icon.py`,
  painted with numpy and Pillow: Apple's rounded-square grid, an aurora over a night sky with stars and
  hills, a black vinyl record with its grooves, glossy wedges and an aurora-green label; every size from
  the 1024-pixel picture; `python3 make-icon.py --preview icon.png` shows it at 1024, 256, 64, 32 and 16;
  without numpy and Pillow, the SE's pixel-art icon from `tools/mac/mkicons.py`), writes Info.plist
  (bundle `org.aurora68.play`; plain HTTP to the home network allowed; the Local Network question's
  text), and signs ad hoc.
- `.github/workflows/macos-player.yml` runs `swift test` (at the top) and `make-app.sh` on GitHub's macOS runner, for
  every push that touches `macos/`, `play/` or `Package.swift`, on any branch (with
  numpy and Pillow in a virtual environment for the icon) and keeps the zipped app as the run's artifact,
  and the icon's preview as **Aurora68-Play-icon**.

Written on 22 November 2026. MPL-2.0, like the rest of the Macintosh side.

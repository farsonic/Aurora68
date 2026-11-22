# Aurora68 Play for macOS

The music server's player on a modern Mac (macOS 13 Ventura or later, Apple silicon or Intel). It is
the companion of the Mac SE's Aurora68 Play: the same library (`tools/music-server`), shown the way a
music app is shown today, and a "Play on" choice that sends the music to the SE through the server's
Live channel.

- **Library:** artists and albums as a grid of covers, an artist's albums, an album with its cover
  and songs (number, title, length). The search field in the toolbar (Command-F) filters artists and
  albums as you type.
- **Playing on this Mac:** the original files, as they are on the server: MP3, AAC and Apple
  Lossless (M4A) and FLAC, played by AVFoundation with real seeking and no gap between songs. Play,
  Shuffle, Add to Queue, Play Next (right-click a song or an album); Previous and Next, a position bar
  you can drag, volume, shuffle and repeat (all, one) in the bar along the bottom.
- **Now Playing:** the song and its cover in Control Center; the keyboard's media keys work. The space
  bar plays and pauses, Command-Left and Command-Right are Previous and Next.
- **Play on Live (Mac SE):** the toolbar's "Play on" menu (or Option-Command-2). Play, the queue and
  the buttons then control the server's Live channel, which every listener hears at once: the SE's
  Aurora68 Play tuned to it (Library > Listen Live). The bar shows what Live plays, and how many
  listen. "Listen here" (the headphones) plays Live on this Mac as well. The Live view in the sidebar
  shows Live's queue (play or remove a song) and its listeners.
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

`build/Aurora68 Play.app` is the result (universal, ad-hoc signed). `swift test` runs the core's tests;
`swift run` starts the app without making the bundle (without its icon and Info.plist).

## How it is made
- `Package.swift`: two targets. **Aurora68Core** is only logic, tested by `Tests/Aurora68CoreTests`:
  `Models.swift` (the server's JSON: `/api/catalog`, `/api/tracks`, `/api/now`), `Address.swift`
  (addresses as typed, URLs with IPv6 in brackets, percent-encoded paths, the discovery answer, the
  preference), `TrackName.swift` (number and title from "01. Artist - Title.m4a"), `PlayQueue.swift`
  (the queue's rules), `MusicAPI.swift` (the HTTP calls). **Aurora68Play** is the app: `App.swift`
  (the app, menus, keys), `AppModel.swift` (connecting, the library, routing to This Mac or Live),
  `Player.swift` (AVQueuePlayer with the playing song and the next; Now Playing and media keys),
  `LiveController.swift` (the Live channel), `Network.swift` (the discovery probe and name lookup,
  BSD sockets), `LibraryViews.swift` and `PlayerViews.swift` (SwiftUI).
- What it asks of the server: `GET /api/catalog`, `GET /api/tracks?album=ID`, `GET /file/PATH`
  (with Range), `GET /art?dir=FOLDER`, `GET /api/now`, `POST /api/live` (header `X-Aurora: 1`),
  `GET /live` (tools/music-server/README.md).
- `make-app.sh` builds for arm64 and x86_64, joins them with `lipo`, makes the icon (`make-icon.py`:
  the SE's Aurora68 Play icon from `tools/mac/mkicons.py`, scaled by whole pixels), writes Info.plist
  (bundle `org.aurora68.play`; plain HTTP to the home network allowed; the Local Network question's
  text), and signs ad hoc.
- `.github/workflows/macos-player.yml` runs `swift test` and `make-app.sh` on GitHub's macOS runner and
  keeps the zipped app as the run's artifact.

Written on 22 November 2026. MPL-2.0, like the rest of the Macintosh side.

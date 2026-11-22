# The video card: YouTube (and any video) on the Mac

22 November 2026. The owner asked for a way to play YouTube videos, "elegant", through a NuBus card that
exists only in the Pi, and portable to Alice68 (the Amiga side of the same Emu68 family). This is the
design; GitHub issues #22 to #26 are its pieces.

## Why the work is split this way
YouTube needs TLS, JavaScript that YouTube changes to unlock its streams, and video in H.264, VP9 or AV1.
The kernel runs on the bare Pi: it has no operating system, so no TLS library worth the name, no
JavaScript, and no access to the VideoCore's hardware H.264 decoder (that needs Raspberry Pi OS's
firmware interfaces). A Cortex-A53 core cannot decode H.264 in software at a useful size either.

MPEG-1 is the opposite: a 1993 format, cheap to decode (a Pi 3 core decodes 640x360 at 30 frames a
second in plain C), with a small public decoder (pl_mpeg, MIT licence, one C file). So:

- **The helper** (a computer on the same network: the owner's Mac, a Docker container, any Linux box)
  does everything modern: search, the YouTube-specific unlocking (yt-dlp, kept up to date there), and
  conversion to MPEG-1 video with MP2 audio at the size the Mac will show (ffmpeg).
- **The video card** (in the Pi) receives that stream, decodes it on a spare core, scales it, and puts
  the picture where the Mac's window is, and the sound into the existing sound card.
- **The Mac** asks for things and shows a window. It never touches a byte of video.

## The helper dials in
The helper opens a connection to the Pi's remote-control port (port 8068, or 80 on the wired port,
with the remote control's token, docs/13) and keeps it open: `GET /video/attach`. The kernel writes one
line of JSON down it for each thing the Mac asks for, and the helper answers on new connections:

| The kernel asks (a line on the attach connection) | The helper answers |
|---|---|
| `{"op":"search","q":"...","id":N}` | `POST /video/results?id=N`: JSON, up to 30 entries (title, channel, length, id) |
| `{"op":"play","video":"...","w":W,"h":H,"start":S,"id":N}` | `POST /video/stream?id=N`: the MPEG program stream (MPEG-1 video, MP2 audio), as long as it lasts, flow-controlled by TCP |
| `{"op":"stop","id":N}` | closes that stream |

So the kernel needs no outgoing connection and no TCP client: its HTTP server (psmac_remote.c) and its
receive flow control (docs/08 item 89) are all it uses. The Mac needs no network at all for video:
without Riptide or MacTCP it still plays. The only setting is on the helper: the Pi's address and
token, the same two `aurora68.py` uses. Pause and seek are the kernel's own (it stops reading the
stream; a seek is a new `play` with `start`).

`/video/attach` is refused without the token, and only one helper is attached at a time (a second
replaces the first). A video ID or search text goes to the helper as text and is never run as anything.

## The card
A virtual NuBus card in slot $B with its own declaration ROM (board name "Aurora68 Video") and its own
window, so it does not crowd slot $9:

- **A command block:** the Mac's requests (search, play with a rectangle, pause, resume, seek, stop,
  volume) and the card's answers (state, position, length, frames shown and dropped, the helper attached
  or not, the last error in words).
- **A results area:** the search results as records the Mac lists.
- **Where the picture goes**, chosen per play:
  - *the HDMI screen*: straight into the frame buffer, inside the rectangle the Mac gives (its window's
    content in global coordinates); the Mac draws nothing there and tells the card when the window moves,
    resizes or is covered. Full colour at the screen's depth.
  - *the SE's own screen*: the kernel dithers each frame to one bit (ordered dithering) into the same
    rectangle of the SE's screen buffer, at the rate the SE can show (about 15 frames a second at
    320x240).
- **The sound** goes into the sound card's ring (docs/08 item 74), so the SE's speaker and HDMI both
  play it.

The kernel side is in the shared Emu68 tree: the stream intake, the decoder, the scaler and ditherer,
the audio path and the command block format are one C module (`psmac_video.c`), with the NuBus card as
a thin layer. An Alice68 build keeps the module and puts a Zorro (autoconfig) card or an Amiga library in
place of the NuBus layer: the helper and the protocol are the same.

## On the Mac
- **Aurora68 Video**, a small program: a search box, a list of results, a window that plays. Play,
  pause, a position slider, a size menu (it asks the helper for that size), full screen on the HDMI
  screen. It only writes requests to the card and draws its frame and controls.
- **A QuickTime component** later (docs/15, issue #13): MPEG-1 files and streams in MoviePlayer and any
  QuickTime program, through the same card.

## Testing without YouTube
The helper has a file mode: `aurora68-video-helper --file movie.mp4` serves one local file as "search
results" and converts it the same way. ffmpeg makes test videos (`testsrc`, a tone), so the whole chain
is tested in the QEMU testbed (helper on the host, through pinet) before any YouTube is involved.

## Pieces (GitHub issues)
1. The helper: Python service and Docker image (issue #22).
2. The card and the decoder in the kernel (issue #23).
3. Aurora68 Video, the Mac program (issue #24).
4. The QuickTime component (issue #25, with #13).
5. Alice68: the same module behind an Amiga card or library (issue #26).

# The video card: YouTube (and any video) on the Mac

22 November 2026. The owner asked for a way to play YouTube videos, "elegant", through a NuBus card that
exists only in the Pi, and portable to Alice68 (the Amiga side of the same Emu68 family). This is the
design; GitHub issues #22 to #26 are its pieces.

The first version of this document (the same day, commit 45ae177) had the helper dial in to the kernel's
remote-control port. The owner turned that down: the remote control is for debugging, it may not be
switched on, and it probably should not be on a machine in everyday use. So video now goes the way
internet radio already goes in Aurora68 Play: the Mac's own program fetches it over the Mac's own TCP/IP,
and the card only decodes. Nothing in the video path touches the remote control, its token or its port.

## Why the work is split this way
YouTube needs TLS, JavaScript that YouTube changes to unlock its streams, and video in H.264, VP9 or AV1.
The kernel runs on the bare Pi: it has no operating system, so no TLS library worth the name, no
JavaScript, and no access to the VideoCore's hardware H.264 decoder (that needs Raspberry Pi OS's
firmware interfaces). A Cortex-A53 core cannot decode H.264 in software at a useful size either.

MPEG-1 is the opposite: a 1993 format, cheap to decode (a Pi 3 core decodes 640x360 at 30 frames a
second in plain C), with a small public decoder (pl_mpeg, MIT licence, one C file). So:

- **The helper** (a computer on the same network: the owner's Mac, a Docker container, any Linux box)
  does everything modern: search, the YouTube-specific unlocking (yt-dlp, kept up to date there), and
  conversion to MPEG-1 video with MP2 audio at the size the Mac will show (ffmpeg). It is a small web
  server.
- **The Mac's program, Aurora68 Video,** finds the helper, asks it for searches and streams over plain
  HTTP through MacTCP or Riptide, and copies the stream's bytes into the card. It never decodes anything.
- **The video card** (in the Pi) decodes the bytes it is given on a spare core, scales the picture into
  the Mac's window, and sends the sound to the existing sound card.

## The helper: a small web server
`tools/video-helper/aurora68_video_helper.py`, Python 3 with only the standard library, calling the
programs `yt-dlp` and `ffmpeg`. Port 8069, TCP for HTTP and UDP for discovery. (It is next to the
remote control's 8068 only in numbering: the two share nothing, and run on different machines.)

| Request | Answer |
|---|---|
| `GET /info` | `{"name", "version", "mode": "youtube" or "files"}` |
| `GET /search?q=TEXT` | `{"results": [{"video", "title", "channel", "seconds"}, ...]}`, up to 30, or `{"error": "..."}` in words |
| `GET /play?video=ID&w=W&h=H&start=S` | an MPEG program stream: MPEG-1 video at W x H (fitted and padded black, 25 frames a second, no B-frames), MP2 at 44.1 kHz stereo, 128 kbit/s. No Content-Length: it ends when the video does, and the Mac closing the connection stops it (the helper kills its ffmpeg) |

- **HTTP/1.0, one request per connection,** which is what the Mac's code already does for radio
  (`mac/piplay/radio.c`): the same request writer and header reader serve.
- **Flow control is TCP's.** While the Mac is paused, or the card's ring is full, the Mac stops reading,
  the helper's write blocks, and ffmpeg waits. No protocol of our own for pause.
- **Seek** is a new `/play` with `start` in seconds. Pause and resume are the Mac's own (stop reading,
  read again).
- **Discovery:** a UDP datagram `AURORA68-VIDEO?` sent to port 8069 (the Mac broadcasts it) is answered
  with the `/info` JSON plus `"port"`, so the owner types no address. If discovery cannot work (a Docker
  container on a bridge network, another subnet) the program takes an address typed in.
- **A token, if wanted:** `--token SECRET` makes the helper refuse (403) any request without the header
  `X-Aurora-Token: SECRET`. Off by default: on a home network the helper shows public videos to anyone
  who could watch them anyway. It is the helper's own setting, nothing to do with the Pi's.
- **Safety:** a video ID must be letters, digits, `-` and `_`, and goes to yt-dlp as one argument, never
  through a shell; search text likewise. In file mode (`--file a.mp4 --file b.mkv`) only the files named
  on the command line can be played, by index (`file:0`), never by path.
- **Sizes:** at most 1280x720, even numbers; the bit rate follows the size (about 0.09 bit per pixel per
  frame, 300 to 2,500 kbit/s): 320x240 is about 430 kbit/s of video, 512x342 about 970.

Running it on a Mac (Homebrew), in Docker, and updating yt-dlp: `tools/video-helper/README.md`.

## On the Mac: Aurora68 Video
A small program, like Aurora68 Play in shape and in code:

- **Finding the helper:** a UDP broadcast at start-up through the same UDP calls Aurora68 Play's mDNS
  uses; the first answer is used and its name shown; File > Helper... takes an address (and token) by
  hand, kept in the program's preferences.
- **Search:** a search box and a list of results (title, channel, length).
- **Play:** a window with Play, Pause, a position slider (a seek is a new `/play`), a size menu (it asks
  the helper for that size), and, with an HDMI screen, full screen there.
- **The stream:** a TCP connection to the helper, read in the program's idle time and on its own
  asynchronous completion routine, as radio is; each block read is handed to the card's ring. The
  program tells the card where its window's content is (global coordinates), whether it is covered, and
  when it moves, resizes or closes.
- **Without a helper:** the window says how to start one, in two lines.
- The Mac's Human Interface Guidelines (1992); fits 512x342; System 7, and System 6 if practical.

The network work is the Mac's: MacTCP or Riptide, Ethernet or WiFi, IPv4 or IPv6 (through `.IPP6` where
present, as Aurora68 Play does). The kernel's remote control can be off.

**The rate this needs.** 320x240 is about 560 kbit/s with its sound, 70 KB a second; Aurora68 Play
already carries 320 kbit/s MP3 radio this way. Larger sizes are for the HDMI screen and want measuring
through MacTCP on the real machine before they are offered by default (issue #24's testing).

## The card
A virtual NuBus card in slot $C with its own declaration ROM (board name "Aurora68 Video") and its own
window, so it does not crowd slot $9. (Slot $B, this document's first choice, is where the IIci's
built-in video answers, $FBB08000, so the ROM does not map its space one to one; $C, $D and $E are the
IIci's real NuBus slots and empty here. Built 22 November 2026: docs/08 item 98.) The window and its
layout are `emu68/src/psmac/psmac_video.h`, which the Mac's program follows:

- **Finding it:** the Slot Manager's sResource of type {$7050, 3, 1, $4156 'AV'}, name "Aurora68_Video",
  no driver. The window is the slot's first megabyte: $FC000000 in 32-bit mode, $C00000 in 24-bit mode.
- **A stream ring** of 512 KiB the Mac writes into (the same producer/consumer pair of counters as the
  sound card's stream channel): the Mac adds bytes and advances `head`, the card decodes and advances
  `tail`. A full ring is what pauses the Mac's reading, and through TCP the helper.
- **A command block:** the Mac's requests (start, pause, resume, stop; a seek is stop, the new stream's
  bytes, start), its picture's place (screen, rectangle in that screen's pixels, visible, under a
  sequence number) and volume; the card's answers (state, the stream's size and rate, position, pictures
  decoded, shown and not drawn, times dry, decode and draw times, its share of the core, and the last
  error in words).
- **Where the picture goes:** into the Mac's own frame buffer of the screen chosen, inside the rectangle,
  at whatever depth that screen is in. The screens' own machinery carries it on from there, so a
  screenshot of the Mac shows it and the Mac stays in charge of what covers what (its program says "not
  visible" while the window is covered or dragged):
  - *the SE's own screen* (screen 0 on the SE board): one bit, each picture dithered by an ordered
    (Bayer) threshold; the rows that changed go to the SE as all its screen rows do. Both screens are
    wanted (the owner, 22 November 2026): the SE's 512x342 one-bit screen will look rough, but it is the
    proof of concept, needs no HDMI screen, and came first.
  - *the HDMI screen* (screen 1 on the SE board, screen 0 on the solo board): millions and thousands of
    colours directly, 256, 16 and 4 colours through the Mac's own colour table with an ordered dither.
- **The sound** goes to the sound mixer beside the Mac's own (HDMI or the jack; the SE's speaker with
  `mac.sestream=on`, as for Aurora68 Play), and the pictures are timed against the sound actually played;
  without sound, against the Pi's timer.

The card does no networking: everything it decodes was put in its ring by the Mac. That keeps it
independent of the Pi's network and of the remote control, and it means anything that can fill the ring
can use it: a QuickTime component reading an MPEG file from disk (issue #25) is the same card with a
different producer.

The kernel side is in the shared Emu68 tree: the ring and command block format, the demultiplexer, the
decoder, the scaler and ditherer and the audio path are one C module (`psmac_video.c`), with the NuBus
card as a thin layer. An Alice68 build keeps the module and puts a Zorro (autoconfig) card or an Amiga
library in place of the NuBus layer; the helper and its HTTP interface are the same, and the Amiga's own
TCP/IP (bsdsocket.library) fetches the stream as the Mac's does.

## Testing without YouTube
The helper has a file mode: `--file movie.mp4` serves local files as "search results" and converts them
the same way. ffmpeg makes test videos (`testsrc`, a tone), so the whole chain is tested in the QEMU
testbed (helper on the host, reached through pinet as the radio stations are) before any YouTube is
involved; until the Mac's program exists, `mac.videotest=` feeds a host file into the card's ring instead.
`tools/video-helper/test_helper.py` tests the helper on its own: discovery, `/info`, `/search`,
`/play` checked with ffprobe (MPEG-1 at the size asked for, MP2 at 44.1 kHz stereo), a stopped play
leaving no ffmpeg behind, refusals, the token.

## Pieces (GitHub issues)
1. The helper: Python service and Docker image (issue #22). Done 22 November 2026 (file mode tested
   here; a real YouTube search and play is the owner's to try, not the build environment's).
2. The card and the decoder in the kernel (issue #23). Built 22 November 2026 and tested in both
   testbeds with a test feeder (`mac.videotest=`, docs/08 item 98); the Pi's own speed is still to measure.
3. Aurora68 Video, the Mac program (issue #24).
4. The QuickTime component (issue #25, with #13).
5. Alice68: the same module behind an Amiga card or library (issue #26).

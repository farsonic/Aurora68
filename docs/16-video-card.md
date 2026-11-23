# The video decoder card: QuickTime, MPEG and any video on the Mac

22 November 2026. The owner asked for a way to play YouTube videos, "elegant", through a NuBus card that
exists only in the Pi, and portable to Alice68 (the Amiga side of the same Emu68 family). This is the
design; GitHub issues #22 to #26 are its pieces.

**Renamed the same evening.** The owner: call it a "video decoder card". The board is "Aurora68 Video
Decoder" in its declaration ROM (it was "Aurora68 Video", its sResource "Aurora68_Video"; the sResource is
now "Aurora68_Video_Decoder", its type and IDs unchanged), the QuickTime extension is "Aurora68 Video
Decoder", the API document `docs/video-decoder-api.md`; the Mac program stays "Aurora68 Video". Older text
below says "the video card" for the same thing.

**And redirected (22 November 2026, late evening).** "This video card should leverage the Pi hardware and
have no external dependencies. Think of it as an in-box video processing card that integrates into
QuickTime and is also available to other programs through traditional means on classic Macs." And:
"It's essentially another NuBus card"; "we will tackle YouTube later". So the helper below is an optional
extra, not the card's purpose; the card got a driver in its own ROM and a frame decoder that QuickTime's
decompressor components use, and the order of work became: QuickTime's legacy formats on the Pi (issue
#13), MPEG-1 from local files (issue #25), a study of the modern formats, and the programming interface.
Those are the sections from "The in-box card" on; the sections before them are the first design, kept as
written.

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

## The in-box card (22 November 2026, late evening)
What the owner's three sentences made of the card, as built that night (docs/08 item 110):

- **A NuBus card of the period.** Slot $C's declaration ROM (`tools/mac/mkvideodeclrom.py`, compiled into
  the kernel as `psmac_video_declrom.h`) names the board "Aurora68 Video Decoder" (vendor "Aurora68
  project", revision 2, part "A68-VDEC") and carries the card's driver, ".A68Video" (`mac/a68video.s`,
  886 bytes), in its functional sResource's driver directory. With no sRsrcFlags the Slot Manager opens it
  at start-up, as it opens PiInput's, and the log says `[MAC] A68Video: the Aurora68 Video Decoder card's
  driver is open (stream channel, frame decoder)` before the first extension loads. The sResource's type
  ({$7050, 3, 1, 'AV'}) did not change, so anything that looked for the card still finds it.
- **The traditional means:** the driver's Control and Status calls, from 100 up (Control 1 is the Device
  Manager's killCode): a frame request, frame decoding on or off, the stream's commands, bytes, place,
  volume and end; the card's address and versions, its formats, the stream's state, the frame decoder's
  counts. `docs/video-decoder-api.md` is the reference, written for a programmer of 1993.
- **The frame decoder** (`psmac_vframes.c`, beside the stream in the same window, from offset $1000): eight
  request slots of 128 bytes; a request opens a session (a format, a size, a depth, the ImageDescription),
  decodes a picture into it and draws it into a pixel map, or closes it. The decoders are portable C with
  no kernel calls (`psmac_vcodec.h`: `psmac_cinepak.c`, `psmac_rpza.c`, `psmac_qtrle.c`, `psmac_smc.c`),
  each written from the format's public description; `tools/vcodec_test.sh` (in `make -C mac check`) has
  FFmpeg encode test movies in each and compares the card's decoding with FFmpeg's, picture for picture:
  Cinepak (two kinds of movie), Apple Video (two), Animation at 16, 24 and 32 bits, Graphics (two), all
  identical byte for byte. The first Graphics run differed from the second picture on: its colour caches
  start again from entry 0 in every picture (their contents stay), which the description does not say.
- **Drawing** goes into whatever pixel map the request names, at its depth, inside a clip rectangle,
  scaled by the nearest pixel if asked: a screen (the SE's own at one bit, slot 9's, the HDMI screen) or a
  GWorld in the Mac's memory. The kernel gained the means to reach any of them from a 68k logical address
  (`psmac_mac_span` and `psmac_mac_copy` in `psmac_mbox.c`: RAM page by page through the MMU shim's table,
  and the screens' frame buffers by the addresses their GDevices use). Each format keeps its whole last
  picture in the session (all four leave unchanged blocks or lines out), so what is drawn is right whatever
  happened to the destination; and when the destination is the same as last time only the part the picture
  changed is drawn, as QuickTime's own decompressors draw only the blocks a picture brings.
- **QuickTime** (`mac/vdec`, the "Aurora68 Video Decoder" extension, type 'thng'): four 'imdc' components,
  manufacturer 'A68v', for 'cvid', 'rpza', 'rle ' and 'smc ', registered by QuickTime from the Extensions
  folder. Each declines registration where ".A68Video" does not open. PreDecompress opens a card session
  and asks for the destination's own depth, the whole picture as one band and no scaling or masking of its
  own (QuickTime decompresses into an offscreen buffer and copies through the mask itself when a window is
  covered or the movie is scaled); BandDecompress passes the picture, the pixel map and the place through
  the driver. Each component opens QuickTime's own decompressor of its type beside itself and hands it
  everything it does not do: all of it when frame decoding is off (`mac.vdecode=off`, or the driver's
  Control 101), a destination depth or picture the card refuses, a picture loaded in pieces, the selectors
  it does not implement. That is "turning it off falls back to QuickTime's own", and it was tested.

### Found on the way
- The component's GetComponentInfo glue used selector 6 (GetComponentListModSeed) for 5: the stack was
  left five arguments deep, and the System bombed with "bad F-Line instruction" at $100 while QuickTime
  registered the components. The kernel's crash record (`[CRASH]`, psmac_crash.c) showed a jump to low
  memory from the component's code; the selectors are now checked against the Component Manager's
  interface, one by one.
- StripAddress's result came back only in D0; GCC reads a pointer result from A0 (mac/c/README.md), so the
  ImageDescription was read from a stale address and its type was 0. Both registers now.
- The driver used A1 after the Device Manager gave it the DCE there, and IODone wants the DCE back in A1:
  System error 14 ("I/O system error") at MoviePlayer's first picture. A0 and A1 are now kept.
- MoviePlayer 2.5 asks for its personalisation once (name and organisation), and Cancel quits it.

### Measured in the testbed
The solo board under QEMU (System 7.1, QuickTime 2.5.1, MoviePlayer 2.5 with Movie > Play All Frames; FFmpeg's
test movies, 320 x 240 at 60 frames a second, five seconds). The component times every BandDecompress with
Microseconds, whoever decodes the picture, so the same number is had with the card and with QuickTime's own
decompressor (`mac.vdecode=off`); the card's log gives its own share.

| Format, depth | the card: a picture in BandDecompress | of which on the Pi | QuickTime's own |
|---|---|---|---|
| Cinepak, 1 bit | 1,028 us (30.6 pictures a second) | about 200 us decoding, 150 drawing | 339 us (31.4 a second) |
| Apple Video, 1 bit | 1,169 us | about 200 and 150 | 672 us |
| Animation, 1 bit | (not run through the card) | | 343 us |
| Graphics, 1 bit | (not run through the card) | | 745 us |
| Cinepak, 8 bits | 1,296 us (42.8 a second) | | 2,207 us (41.5 a second) |
| Apple Video, 8 bits | 1,766 us | | 354 us |
| Animation, 8 bits | | | 313 us |

Pictures (screenshots of the Mac, in the scratchpad, not committed): Cinepak at 8 bits on the solo board's
screen (`solo-cinepak-8bit.png`), Apple Video at 1 bit (`solo-rpza-1bit.png`), Cinepak on the SE's own
screen in MAME (`se-cinepak-1bit.png`).

### What the numbers mean
The 68k of this machine is not a 68000: Emu68 translates QuickTime's decompressors into ARM code, and where
they are simple (Apple Video, Animation, Graphics) or have a fast path of their own (Cinepak straight to one
bit) they already take a third of a millisecond. The card's round trip in the testbed is about a millisecond,
of which the Pi's work is a third: the rest is the call through the Device Manager and the waiting of four
QEMU threads on four host cores (the testbed's timing, not a Pi's). Where QuickTime's own code works hardest,
Cinepak into a colour screen, the card is faster already (1.7 times). On the SE the 68k is the same Emu68,
on a Pi with no QEMU in between, so the round trip should shrink and the Pi's share stay; the owner's SE is
where this is to be measured. The next steps that would make the card faster in every case: asynchronous
decompression (codecCanAsync: the 68k goes on with the movie while the Pi decodes; QuickTime 2.5's ICM
supports it for hardware decompressors), and a choice per format and depth of whether to hand a movie to
the card, made from these measurements.

## Modern formats: a study (22 November 2026)
The owner's third item: H.264 video ('avc1') and AAC sound ('mp4a') in .mp4 and .mov files from a camera or
a phone. QuickTime 2.5 reads the MPEG-4 file format's atoms as a QuickTime movie (they are the same
structure) but has no decompressor for either; a component for each, decoding on the Pi, would make such a
file play in MoviePlayer like any other. The question is what decodes them on the Pi.

- **The VideoCore IV's hardware decoder.** A Pi 3 decodes H.264 up to 1080p at 30 frames a second in the
  GPU's video block, which the GPU's firmware (start.elf) drives; the ARM side talks to it through VCHIQ, a
  message queue in shared memory announced to the firmware by a mailbox property tag, and on top of VCHIQ
  through MMAL (the "vc.ril.video_decode" component, its ports, its buffer headers) or OpenMAX IL. The
  firmware runs whatever is on the ARM side, so a bare-metal kernel can use it as Linux does. What exists:
  Linux's VCHIQ driver (`drivers/staging/vc04_services`, dual BSD-3-Clause/GPL-2.0, so usable here under
  BSD), Raspberry Pi's userland MMAL client library (`raspberrypi/userland`, BSD-3-Clause), and Circle's
  bare-metal port of VCHIQ in its `addon/vc4` (Circle is GPL-3.0, which cannot be combined with the
  kernel's GPL-2.0-only Alice68 modules; it shows that it can be done, not code to take). The work: VCHIQ's
  slots, its doorbell interrupt and service opening; an MMAL client for the decoder (create, set the
  input port's format to H.264, enable, pass buffers in and get YUV pictures out, zero-copy through
  VideoCore memory); then the 'avc1' component, which hands QuickTime's samples (length-prefixed NAL units
  and the avcC record of the sample description) to it and converts its pictures into the Mac's pixel map
  as the frame decoder does. A few weeks, and none of it can be tested in this testbed: QEMU's raspi3b has
  no VideoCore firmware and answers no VCHIQ. It needs the owner's Pi (gpu_mem of 128 MB or more; H.264
  needs no codec licence key on a Pi 3, MPEG-2 and VC-1 do). It is the only route to phone video at its
  own size: a Pi 3's A53 core cannot decode 1080p H.264 High profile in software in real time.
- **Software H.264.** Cisco's openh264 (BSD-2-Clause, compatible) decodes Constrained Baseline, and in its
  later versions much of Main and High; it is C++ with assembly for little-endian ARM, and this
  kernel is big-endian AArch64 and C, so its assembly would not serve and its C++ would need a runtime the
  kernel lacks. Android's h264bsd is Apache-2.0, which the FSF holds incompatible with GPL-2.0 (the reason
  Apple's ALAC decoder was not used, docs/08 item 105); libavcodec is LGPL and may not be copied. At the
  sizes a Pi 3 core decodes in plain C (about 320 x 240 to 480 x 270 of Baseline at 25 frames), software
  decoding would serve video made small for the Mac, which the helper can already make as MPEG-1.
- **AAC.** FAAD2 is GPL-2.0-or-later: it could live in a GPL module beside the Alice68 ones, but the
  kernel's MPL side would have to call it through an interface of its own; FDK-AAC's licence is not
  GPL-compatible; Helix AAC's (RPSL/RCSL) neither; opencore-aac is Apache-2.0. AAC also carries patent
  licensing in some countries. The VideoCore has no AAC decoder of its own to borrow (its audio is PCM).
  The practical answer is the one Aurora68 Play took: convert on a computer (the music server, the helper)
  when it is there, and leave AAC out of the in-box card.
- **Recommendation.** H.264 through VCHIQ and MMAL as a hardware-bench task with the owner's Pi, its
  QuickTime side ('avc1' component, the same request slots with a hardware session) designed now and built
  when VCHIQ answers; AAC not in the card. Not started in this round: Phase C stops at this study.

## Pieces (GitHub issues)
1. The helper: Python service and Docker image (issue #22). Done 22 November 2026 (file mode tested
   here; a real YouTube search and play is the owner's to try, not the build environment's).
2. The card and the decoder in the kernel (issue #23). Built 22 November 2026 and tested in both
   testbeds with a test feeder (`mac.videotest=`, docs/08 item 98); the Pi's own speed is still to measure.
3. Aurora68 Video, the Mac program (issue #24). Its network half was set aside on 22 November 2026 ("we
   will tackle YouTube later"); its local player is part of issue #25's work.
4. The QuickTime components (issue #25, with #13). Issue #13's decompressors built 22 November 2026: the
   card's driver in its ROM, Cinepak, Apple Video, Animation and Graphics on the Pi ("The in-box card").
   MPEG-1 from local files (issue #25) next.
5. Alice68: the same module behind an Amiga card or library (issue #26).
6. Modern formats (H.264, AAC): studied 22 November 2026 ("Modern formats: a study"), not built.
7. The programming interface: `docs/video-decoder-api.md` (22 November 2026).

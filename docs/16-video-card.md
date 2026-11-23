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

## MPEG-1 from the Mac's own disks (23 November 2026, issue #25)
- **Aurora68 Video** (`mac/a68player/video.c`, C with the kit): the card's own player. File > Open... (or a
  double-click on an MPEG file in the Finder: its bundle takes types 'MPEG' and 'MPG ') reads an MPEG-1
  program stream from disk straight into the card's ring (a PBRead whose buffer is the ring, so the 68k
  copies nothing) in the event loop's idle time, 32 KB at a time while there is room; the driver starts,
  pauses, resumes and stops the stream and places the picture. The picture is the card's: the window's
  content rectangle in its screen's pixels, "not visible" while the window is behind, dragged or under a
  menu; the kernel now draws the last picture again when the place changes while the stream is paused or
  dry (`psmac_video.c`), so a paused film follows its window. Play/Pause, a position bar (a click seeks),
  the time and the length, Normal and Double Size, three volumes, and Card > Decode QuickTime Movies on the
  Card (the driver's Control 101, ticked from its Status 100). Without the card the window says so in two
  lines.
- **Seeking without an index:** stop, the film's first 2 KB pack (its system header and the video's sequence
  header, which encoders write once, at the start), then the file from the 2 KB pack boundary nearest the
  click, start. The first try without the header pack ended in "no MPEG-1 video found at the start of the
  stream". The pictures until the next key frame are decoded from a reference that is not theirs (under a
  second of blocks at FFmpeg's 25-frame groups). The length comes from the first pack's mux_rate when it is
  believable (8 KB to 2 MB a second; FFmpeg wrote 55 MB a second for a test pattern's film) and otherwise from
  the bytes the decoder has used against its position after three seconds.
- **Tested** (solo board, System 7.1; a 12-second FFmpeg film, MPEG-1 320 x 240 at 25 frames and MP2,
  `-bf 0 -g 25`, the helper's parameters): opened through Standard File and from the Finder; played to the end
  (300 of 300 pictures shown, never dry, 0.8-1.0 ms to decode and 0.7-0.8 to draw a picture under QEMU, 1.3 to
  4.7 per cent of core 1); paused, the window dragged (the card told "hidden" during the drag) and the last
  picture redrawn at the new place; a click in the middle of the bar played the second half (150 pictures,
  5.9 s); the Card menu's tick followed the driver's switch. Screenshots in the scratchpad: `video-playing.png`,
  `video-after-seek.png`, `video-paused-moved-menu.png`. The film's sound was not heard: the testbed has no
  sound output, and the card then times the pictures by the Pi's clock.
- **QuickTime and MPEG-1 (MoviePlayer opening a .mpg):** designed, not built. A movie import component
  ('eat ', subtype 'MPEG') would index the program stream at import (pack by pack: each video picture's
  start, type and size; each MP2 frame) and make a movie of two tracks: video whose samples are the coded
  pictures, of a compression type of the card's ('mpg1'), with I-pictures marked as sync samples, decoded by a
  fifth 'imdc' component through a frame session of the card around pl_mpeg's video decoder; and sound whose
  samples are MP2 frames, played through a sound decompressor ('sdec') for MP2 that hands frames to the card
  and gets PCM back. What does not fit QuickTime 2.5: it has one time per sample, no separate decoding and
  display order (that came with QuickTime 6), so a film with B-pictures is decoded in coded order and shown
  one picture late, which a decoder that holds one picture back can hide but not undo on a seek; and the
  sound decompressor is a component of the Sound Manager 3.1 interface, a second piece of work as large as
  the import. Until those exist, an MPEG file opens in Aurora68 Video from the Finder, and MoviePlayer opens
  everything else through the card's decompressors. (Built the same day, with the B-pictures and the sound done
  differently: "MPEG-1 in QuickTime" below.)

### Follow-up: Aurora68 Play's music videos (23 November 2026)
The music server now serves a music video as an MPEG-1 program stream at `/vmpeg/ID` (docs/08 item 109), and the
classic Play says it shows no video until the card's program can. The interface for it is the card's driver,
not Aurora68 Video's window: Play opens ".A68Video", takes the window's address from Status 100, pours the
`/vmpeg` stream into the ring as it pours radio into the sound card (its receive chain writing at head, or
Control 111), and drives Control 110 (start, pause, resume, stop), 112 (the picture's place: a rectangle of its
own window, or of the HDMI screen for full screen), 113 (volume) and 114 (end). Nothing in Aurora68 Video needs
to change for that; if Play would rather hand the film over, Aurora68 Video could take a URL in an 'odoc' of a
small text document, which is not built.

## MPEG-1 in QuickTime: MoviePlayer opens .mpg files (23 November 2026, issue #25)
The design of the section before ("QuickTime and MPEG-1"), built, with the changes QuickTime 2.5 asked for: the
B-pictures are put in display order by the import rather than held back by the decoder, the sound is decoded by
the card once, when the film is opened, rather than by a sound decompressor at play time, and the decompressor
catches the card up after a jump, which QuickTime 2.5 does not do. docs/08 item 113 is the record of how it went.

- **The movie import** (`mac/vdec/import.c`, two 'eat ' components of the Aurora68 Video Decoder extension, for
  the file types 'MPEG' and 'MPG ', flags canMovieImportFiles, canMovieImportValidateFile and
  canMovieImportInPlace). QuickTime asks it to make a movie of a file; it reads the file once, 32 KB at a time,
  through the indexer (`mac/vdec/mpgindex.c`: the program stream taken apart packet by packet, the video's start
  codes found, every picture's start and type), and makes the tracks without copying the film: the video
  track's samples are references into the file itself (an alias to it is the media's data reference). Each
  picture is one sample, one frame long at the film's rate (the media's time scale 2500 with samples of 100 at
  25 frames a second, 30000 with samples of 1001 at 29.97), of the card's type 'mpg1', the sequence header in the
  ImageDescription (an extension atom, 'mpgh'); the I-pictures' samples are the sync samples. In a program stream
  a sample's range starts in the middle of a packet and has packet headers and sound inside it: the card takes
  them out (it finds the packet's end by the first system start code, 00 00 01 and a code from B9 up, which the
  video itself never contains).
- **How MoviePlayer 2.5 opens it.** File > Open... lists the MPEG films beside the movies, with a "Convert"
  button where "Open" would be; Convert opens the film in place, in a window with the film's name, with no
  question where to save anything (QuickTime calls the import with none of its flags set, not with
  movieImportCreateTrack, which the first version refused). The film can then be saved as a small movie that
  refers to it (File > Save As..., "Save normally": 4 KB for an eight-second film) or self-contained. A double-click
  in the Finder does not reach the import: given to MoviePlayer (creator 'TVOD') a film is refused with
  "Couldn't open the file because the file is not a movie file", MoviePlayer's own check; the films keep Aurora68
  Video's creator, and a double-click plays them there. Standard File's preview makes a movie of each film
  selected, so the first selection of a new film is where its sound is decoded.
- **B-pictures.** QuickTime 2.5 has one order for samples, the order they are shown in. MPEG-1 sends each
  reference picture (I or P) before the B-pictures shown before it. The import makes the samples in display order
  and lets the first B-picture after a reference carry the reference's bytes in front of its own (one range of the
  file: they are next to each other there); the reference's own sample comes later, when it is shown. The card's
  'mpg1' session decodes a reference unless it is the one it decoded last (it knows it by a hash of its bytes)
  and shows the last picture of each sample. The other way, the decoder holding one picture back as the design
  first said, would have shown every picture one frame late and could not be put right after a jump.
- **Jumps.** QuickTime 2.5 does not decode from the preceding key frame when it goes to a time: a click in the
  controller handed the decompressor the sample at that time and nothing else (the card's log with
  `mac.vframelog=1`: one picture, a P and a B, decoded against the references of the frame left about 80 pictures
  earlier, and the test pattern's digit came out looking like an 8, a 0 and a 4 laid over each other), and when it is late
  while playing it skips samples the same way. The 'mpg1' decompressor therefore catches the card up itself.
  QuickTime's frameNumber is the sample's number (seen in the same log: 1, 2, 3... while playing, 104 after a
  click near the middle), so the decompressor knows which sample it is given; the import puts its index into the
  ImageDescription (a second atom, 'mpgx': where the film is, its size, every picture's start and type, 4 bytes a
  picture, up to 32,768 pictures), and when the sample is neither the next one nor a sync sample, the
  decompressor reads from the film the samples from the sync sample before it, leaves out the B-pictures alone,
  and has the card decode them undrawn first (`mi_catchup`; the same arithmetic the host test checks). After
  that the click near the middle showed picture 103, matching FFmpeg's 103 in 99.7 per cent of the pixels.
- **The fifth decompressor**, 'mpg1' (`mac/vdec/vdec.c`), works as the other four: a session per movie, every
  picture handed through the driver, drawn by the card at the destination's depth. On the Pi
  (`psmac_plmpeg.c`) the session is pl_mpeg's video decoder used one picture at a time below its own stream
  reader: the sequence header decoded once, each picture with plm_video_decode_picture.
- **The sound** becomes 16-bit stereo PCM at the film's own rate, decoded by the card while the import reads the
  file (a sound session of the frame decoder, `A68V_OP_SOUND`), written to a file in the Preferences folder's
  "Aurora68 Video Sounds" ("film.mpg sound"), to which the sound track refers. The file is used again the next
  time the same film is opened (its header names the film's size, date, folder and name), and the folder keeps
  eight others. The PCM lies in blocks of half a second, four bytes apart, so that each is a chunk of its own:
  the first version made the whole sound one chunk, and the first press of Play then left the movie on its first
  two pictures, QuickTime asking for them again and again; with half-second chunks it played at once. The sound
  track is no longer than the pictures (MP2's last frame runs on by up to 26 ms; a sound track that outlasted the
  video left the window white at the end). Why not a sound decompressor for MP2: QuickTime 2.5's sound media
  takes uncompressed sound or a format of a fixed compression ratio (the Sound Manager's CompressionInfo: so many
  bytes a packet, always), whose bytes lie one after another in a file. MP2 at 44.1 kHz has frames of 417 and 418
  bytes, and in a program stream they are spread between the video's packets, so neither the film's own bytes
  nor an 'sdec' could have served; a copy of the sound in another file was needed either way, and once copied,
  PCM needs nothing at play time. The cost is disk space, 10 MB a minute, and the first opening's decoding.
- **Without the card** (no card, `mac.video=off`, or the card's decoding off: `mac.vdecode=off`, Aurora68 Video's
  Card menu) the import still opens the film, with the video track disabled and a text track in its place, white
  on black: "This MPEG-1 film needs the Aurora68 Video Decoder card: QuickTime has no MPEG-1 decoder on a 68k
  Macintosh." or, with the card there and its decoding off, how to switch it on. No sound track is made: the card
  decodes the sound too.
- **Tested** (solo board under QEMU, System 7.1, QuickTime 2.5.1, MoviePlayer 2.5 at one bit; FFmpeg's test
  pattern with a sine tone, 320 x 240, MPEG-1 at 600 kbit/s and MP2 at 128 kbit/s, eight seconds: 25 frames a
  second without B-pictures, 25 with two B-pictures in groups of 12, 29.97 with B-pictures in groups of 15).
  Each opened through File > Open... ("Convert"); with Movie > Play All Frames every picture went through the card
  (200, 200 and 240), the last one shown at the end; pictures compared with FFmpeg's decoding, dithered as the
  card dithers: frame 5 (a B-picture), 21, 103 (after a seek) and 199 agreed in 99.7 per cent of their pixels and
  each matched its own frame better than its neighbours. Played in real time, QuickTime dropped to the key frames
  and the decompressor caught up where it landed between them (the testbed's 68k runs at about 20 MIPS under
  QEMU, a twentieth of a Pi's), and the sound reached the card: with `mac.audiodump=` the dump held the film's
  440 Hz tone, with gaps where the testbed's 68k fell behind (the kernel counted 1,771 underruns of the sound card's ring). Saved as a
  reference movie (before the sound's chunks were made smaller), its sample tables were what the import made
  (200 samples of 100 at 2500 a second, 17 sync samples, one every 12). Without the card and with its decoding off, the text track said so. The import took 2 to
  2.6 s for a film of 440 KB under QEMU, 9 to 12 s the first time (with the sound's decoding and 1.4 MB written).
  Screenshots in the scratchpad: `mpeg-standard-file.png`, `mpeg-opened-1bit.png`, `mpeg-playing-allframes.png`,
  `mpeg-after-seek.png`, `mpeg-end.png`, `mpeg-no-card.png`, `mpeg-vdecode-off.png`.
- **On the host** (`make -C mac check`, `tools/mpg1_test.sh`): FFmpeg's films (with and without B-pictures, 25
  and 29.97, a bare video stream) indexed by the same `mpgindex.c` in 32 KB pieces, every sample decoded by the
  card's code in display order and compared with FFmpeg's decoding (at least 52 dB on every picture: the inverse
  DCTs differ by a step here and there); from every sync sample with the old session and a new one; QuickTime's
  jumps to every sample with the catch-up; each sample twice (a redraw); the sound against FFmpeg's (59 dB once a
  constant gain is allowed for: pl_mpeg's synthesis is 0.2 to 0.7 dB quieter).
- **Not verified:** speed on a Pi (the testbed's 68k is far slower than an Emu68 68k on the SE), the sound heard
  (the testbed has no output), colour screens and the SE board with these films, MPEG-1 from other encoders of the
  period (only FFmpeg's), films longer than a few minutes, and System 6 (the import needs the Alias and Folder
  Managers of System 7).

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
   MPEG-1 from local files (issue #25): Aurora68 Video plays them on the card, 23 November 2026; and the same day
   MoviePlayer and every QuickTime program open them, through the extension's MPEG-1 import, a fifth decompressor
   ('mpg1') and the sound decoded by the card into PCM at import ("MPEG-1 in QuickTime").
5. Alice68: the same module behind an Amiga card or library (issue #26).
6. Modern formats (H.264, AAC): studied 22 November 2026 ("Modern formats: a study"), not built.
7. The programming interface: `docs/video-decoder-api.md` (22 November 2026).

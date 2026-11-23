# The Aurora68 Video Decoder card: programming interface

22 November 2026. This is the interface a Macintosh program uses to put the Aurora68 Video Decoder card
to work, written for programmers of the period's tools as much as for this project's own (Aurora68 Video,
the QuickTime components in the "Aurora68 Video Decoder" extension, later Aurora68 Play's music videos).
The card's design and history are docs/16-video-card.md; the kernel's side is
`emu68/src/psmac/psmac_video.h`, which is the authority on every offset below.

## What the card is
A NuBus card in slot $C that exists only in the Raspberry Pi (`psmac_videocard.c`). Its declaration ROM
says:

| | |
|---|---|
| Board sResource (1) | name "Aurora68 Video Decoder", BoardId $4156 ('AV'), vendor "Aurora68 project", revision "2", part "A68-VDEC" |
| Functional sResource ($80) | type {category $7050, cType 3, drSW 1, drHW $4156}, name "Aurora68_Video_Decoder", driver ".A68Video" (sMacOS68020), MinorBaseOS 0, MinorLength $100000 |

The Slot Manager installs and opens ".A68Video" at start-up (the sResource has no sRsrcFlags, so
fOpenAtStart applies), as for any card that carries its driver. Aurora68 Slots lists the card. With
`mac.video=off` on the Pi's command line the slot is empty.

The card does two things:
- **The frame decoder:** one compressed picture at a time, of a format QuickTime uses (Cinepak 'cvid',
  Apple Video 'rpza', Animation 'rle ', Graphics 'smc ', and since 23 November 2026 MPEG-1 'mpg1', the type
  the extension's MPEG-1 import gives a film's pictures), decoded by a Pi core and drawn into a pixel map
  of the Mac's (a screen's frame buffer or an offscreen GWorld) at that map's depth (1, 2, 4, 8, 16 or 32
  bits), scaled by the nearest pixel when asked, clipped to a rectangle. The QuickTime components use it.
- **The stream channel:** an MPEG-1 program stream (MPEG-1 video, MP2 sound) poured into a 512 KiB ring
  (with, since 23 November 2026, a buffer of 8 MB in the Pi behind it: "Buffering" below)
  and played into a rectangle of a screen, its sound mixed with the Mac's own; a program controls start,
  pause, resume, stop, the picture's place and the volume.

Nothing else is needed on the Mac, not even QuickTime, and nothing outside the Mac and the Pi.

## Opening the driver
```pascal
err := OpenDriver('.A68Video', refNum);    { fails with openErr (-23) when the card does not answer }
```
The driver is never closed by its users (it belongs to the card). Every call below is synchronous
(`PBControlSync`, `PBStatusSync`; `Control` and `Status` with the csParam bytes). An asynchronous call is
carried out at once all the same.

## Control calls
The numbers start at 100, clear of the Device Manager's own (Control 1 is killCode, KillIO).


| csCode | csParam | What |
|---|---|---|
| 100 | a pointer to a frame request (128 bytes, below) | carried out by the card; the request's `result` says how. ioResult is noErr, or ioErr if the card did not answer within three seconds, or all eight request slots were busy |
| 101 | a word: 1 on, 0 off | frame decoding switched on or off. Off: every request is answered 8 ("switched off"), and the QuickTime components hand every picture to QuickTime's own decompressors. `mac.vdecode=off` on the Pi starts it off |
| 110 | a long: 1 start, 2 pause, 3 resume, 4 stop | a stream command; returns once the card has taken it |
| 111 | a pointer, a length (two longs) | stream bytes into the ring, as many as there is room for; the third long (csParam+8) is set to how many were taken. A full ring takes none: try again later (that is how the card paces a program reading a disk or the network) |
| 112 | a pointer to six longs | the stream's picture: screen (0 the first: the SE's own on the SE board; 1 the HDMI screen on the SE board), top, left, bottom, right in that screen's pixels, visible (0: draw nothing, for a window that is covered or being dragged) |
| 113 | a long | the stream's volume, left << 16 \| right, $0100 full |
| 114 | a long: 1 | the stream's last byte is in the ring (the card plays out what it holds and its state becomes "ended") |

## Status calls

| csCode | csParam | What |
|---|---|---|
| 100 | five longs come back | the card's window address in the caller's addressing mode ($FC000000 in 32-bit mode, $C00000 in 24-bit mode), the stream block's version, the frame decoder's version, frame decoding on (1) or off (0), the ring's size |
| 101 | a pointer to 64 bytes | the formats the frame decoder takes, as FourCCs, ending with 0 |
| 102 | a pointer to 384 bytes | the stream block (`struct a68v_block`): state, the stream's picture size, frame rate and sound, its position in ms, pictures decoded, shown and not drawn, decode and draw times, times it ran dry, the core's load, a message in words |
| 103 | a pointer to 64 bytes | the frame decoder's header (`struct a68v_fhdr`): pictures decoded, drawn and refused since start, sessions open, the last second's decode and draw times, the largest picture |

A program that streams at a high rate may write the ring itself rather than through Control 11: the window
address from Status 100, the ring at offset $80000, `head` at $88 (the program's), `tail` at $18 (the card's),
room while head - tail < ring size; bytes first, then head. That is what Control 111 does.

### Buffering, and "Buffering N%" (block version 2, 23 November 2026)
Since 23 November 2026 (docs/08 item 125) the card empties the ring into a buffer of its own in the Pi's memory
(8 MB by default, `mac.vbuffer=MB` on the Pi; 0 the ring alone) as fast as the bytes come, so the room in the ring
is nearly always the whole ring and a program can keep reading the network at its full rate while the film plays;
the ring fills, and holds the program back, only when that buffer is full. A stream starts once it holds
`mac.vprefill=`'s first number of seconds (3 by default), the whole stream, or as much as fits; when the network
runs it dry the card stops its clock (pictures and sound together) and buffers to the second number (2 seconds)
before going on. Nothing changes for a program: the ring, its counters and the commands are as above, and a STOP
empties both the ring and the Pi's buffer. Seconds are the stream's own (its packs' clock against the bytes held),
so they hold for any rate.

The block's version is 2, and the six longs that were spare at $68 say what the card holds; a program that wants to
show "Buffering..." with a figure reads them when `version` >= 2 (Status 102 copies the whole block):

| Offset | Field | |
|---|---|---|
| $1C | state | 1 (buffering) both before the start and while rebuffering; `pos_ms` tells them apart (0 before the first picture) |
| $58 | buffered | bytes held and not yet decoded: the ring's, the Pi's buffer's and the decoder's own |
| $60 | underruns | times the stream ran dry and the card rebuffered |
| $68 | buffer_ms | the stream time those bytes hold, in ms (with sound, the sound waiting in the mixer too) |
| $6C | buffer_pct | buffer_ms against buffer_target_ms, 0 to 100: the N of "Buffering N%" while state is 1 |
| $70 | buffer_size | bytes of the Pi's buffer behind the ring (0: the ring alone); set at the first stream |
| $74 | rebuffer_ms | time spent rebuffering this stream |
| $78 | byte_rate | the stream's bytes a second as the card measured it (0 until known) |
| $7C | buffer_target_ms | what the buffering waits for: the start's seconds, then the rebuffer's |
| $100 | message | while buffering "buffering N%" |

## The frame request
Big-endian longs (the 68k's order), 128 bytes; the driver copies bytes 8 to 127 into a free slot of the
card and back.

| Offset | Field | |
|---|---|---|
| 0, 4 | seq, done | the driver's |
| 8 | op | 1 open a session, 2 decode a picture, 3 close the session, 4 draw the session's last picture again, 5 decode sound (an 'MP2 ' session, below) |
| 12 | session | set by open; given to the others |
| 16 | codec | open: the format's FourCC (the ImageDescription's cType); 'MP2 ' opens a sound session |
| 20, 24 | width, height | open: the picture's size (at most 1024 x 768) |
| 28 | depth | open: the ImageDescription's depth (1-32, 33-40 grey) |
| 32, 36 | src, src_len | decode: the compressed picture's address and length (at most 1 MiB) |
| 40, 44 | desc, desc_len | open: the ImageDescription itself (for an indexed format's colour table), its idSize |
| 48, 52, 56 | dst_base, dst_rowbytes, dst_depth | decode, draw: the pixel map's baseAddr, rowBytes (the flag bits masked off) and pixelSize |
| 60, 64 | dst_x, dst_y | where the picture's top-left pixel goes, in the map's pixels counted from baseAddr (dstRect.left - bounds.left, dstRect.top - bounds.top) |
| 68, 72 | dst_w, dst_h | its size there (the picture's own for 1:1; anything else is scaled by the nearest pixel) |
| 76-88 | clip_left, top, right, bottom | what may be written, in the same coordinates |
| 92, 96 | ctab, ctab_seed | depths 1 to 8: the map's ColorTable (dereferenced) and its ctSeed; 0 for the standard table of the depth |
| 100 | flags | 1: decode only, draw nothing (a picture QuickTime skips); 2: no ordered dither at indexed depths |
| 104 | result | 0 done; 1 not a format the card decodes; 2 not at that depth; 3 too large; 4 no memory; 5 no such session (or sixteen open); 6 the data was damaged (what could be decoded was drawn; open: the description's MPEG-1 sequence header was); 7 an address the card cannot reach; 8 switched off; 9 no such operation |
| 108, 112 | decode_us, draw_us | the time the Pi took |
| 116 | frame | the program's own number, for the Pi's log |

Addresses are the 68k's logical addresses in the caller's mode. The card reads and writes the Mac's RAM and
screens directly; memory under Virtual Memory must be held (`HoldMemory`) for the call. A session keeps what
a format keeps from picture to picture (Cinepak's codebooks; the whole last picture, since every format here
leaves unchanged blocks or lines out), so decode a movie's pictures in order, from a key frame.

### MPEG-1 pictures ('mpg1', 23 November 2026)
An 'mpg1' session decodes MPEG-1 video (ISO/IEC 11172-2) with pl_mpeg's decoder, a sample at a time. Its open
request's `desc` is an ImageDescription of 86 bytes followed by an extension atom: a long, its size (12 and the
sequence header's length), 'mpgh', then four bytes (version 1; the kind: 1 a bare video stream, 2 a program
stream; the video stream's id, $E0 to $EF, in a program stream; 0) and the sequence header itself, from its
start code 00 00 01 B3, quantiser matrices included. A decode request's data is one sample as the import makes
them: bytes of the film from a picture's start (the sequence or group header in front of it, or its picture
start code) to the next picture's start. In a program stream that range starts in the middle of a packet and
may hold pack headers and other streams' packets; the card keeps only the video stream's bytes. A sample holds
one picture, or a reference picture (I or P) and a B-picture: the session decodes the reference unless it is the
one it decoded last, decodes the B against the two references, and shows the last picture of the sample (the B,
or the reference). That is how B-pictures fit QuickTime 2.5's single order of samples: the import puts a
reference's bytes in front of the first B shown before it, and gives the reference its own sample when it is
shown, which the card recognises (a hash of its bytes) and shows without decoding again. Decode in display
order, from an I-picture's own sample. QuickTime 2.5 does not do that by itself after a jump; the extension's
'mpg1' decompressor reads the samples it needs from the film (the import's index, a second atom 'mpgx' after
'mpgh', which the card ignores) and sends them first with flags 1, decode only. With `mac.vframelog=1` on the Pi
the card logs every picture it decodes: the request's frame number, the bytes, the pictures in the sample (I, P,
B; lower case for a reference shown again without decoding) and whether it was drawn.

### Sound ('MP2 ', op 5, 23 November 2026)
Open with codec 'MP2 ' (no size or depth): a sound session, which decodes MPEG-1 layer II sound (pl_mpeg's
decoder) for a program that wants the PCM, as the import does once for a film's sound track. Op 5 takes
`src`, `src_len`: the next bytes of the elementary stream (any split; a frame cut in two waits for its rest),
and `dst_base`, `dst_rowbytes`: where 16-bit big-endian stereo PCM may go and how many bytes (at most 256 KB a
call). It answers in four fields of the request: `width` the bytes of PCM written, `height` 1 when more PCM is
waiting (call again with `src_len` 0), `depth` the stream's sample rate once known, `dst_w` the channels (2;
a mono stream is given on both). Close it as any session. The PCM is at the stream's own rate, never resampled.

### Drawing
At 32 and 16 bits the picture goes in as it is. At 8, 4 and 2 bits each colour is looked up in the map's
colour table (a table of its nearest entries, made again when the seed changes); an 8-bit picture whose
table is the map's goes in index for index, others with an 8 x 8 ordered dither. At 1 bit (the SE's own
screen) the luminance is compared with an 8 x 8 Bayer threshold, black where it is darker.

## The QuickTime components
"Aurora68 Video Decoder" (type 'thng', in the Extensions folder) holds five 'imdc' components, manufacturer
'A68v', subtypes 'cvid', 'rpza', 'rle ', 'smc ' and 'mpg1' (which has no QuickTime decompressor to fall back
to), and since 23 November 2026 the MPEG-1 movie import, two 'eat ' components for the file types 'MPEG' and
'MPG ' (docs/16 "MPEG-1 in QuickTime"). QuickTime registers them at start-up; each declines
registration when ".A68Video" does not open. A component opens QuickTime's own decompressor of the same type
beside itself and hands it whatever it does not do: everything when frame decoding is off; a sequence whose
destination depth or picture the card refuses; a picture delivered in pieces through a data-loading
procedure; every selector it does not implement. Its log lines go to the Pi's log as
"[MAC] A68 Video Decoder: ...", one when a sequence starts on the card and one when it ends, with the rate
the pictures came at.

## A minimal client (C, MPW style)
```c
short ref; CntrlParam pb; struct a68v_freq r;
OpenDriver("\p.A68Video", &ref);
memset(&r, 0, sizeof r);
r.op = 1; r.codec = 'cvid'; r.width = 320; r.height = 240; r.depth = 24;
r.desc = (long)*desc; r.desc_len = (*desc)->idSize;
pb.ioCRefNum = ref; pb.csCode = 100; *(long *)pb.csParam = (long)&r;
PBControlSync((ParmBlkPtr)&pb);                    /* r.session is now set */
/* each picture: r.op = 2, r.src/src_len, r.dst_* and r.clip_* from the GWorld's or screen's PixMap */
```

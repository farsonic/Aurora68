# Remote control: the Mac's screen, mouse and keyboard over the network, from the Pi

*20 November 2026.* The user asked for a way for an agent (Claude Code or Claude Desktop on their
MacBook) to see and drive the Mac directly, so that a problem can be reported and reproduced
without photographs of the screen. The Pi is the Mac's processor and already owns everything
such a remote needs: the frame buffer the Mac draws into (it converts it to HDMI, docs/10), the
mouse and keyboard (it feeds `.PiInput`'s ring, docs/09), the kernel's log (Aurora68 Log's ring) and the
68k itself. So the remote lives in the kernel, needs no program on the Mac, and keeps working
when the Mac has hung or shows a system error. This note is the design; what was built and how
it was tested follows it.

## The pieces

```
  MacBook                                    Pi (Aurora68 kernel, core 3)                    Mac (68k, core 0)
  Claude ── MCP (stdio) ── aurora68-mcp.py                                                   
                              │ HTTP + token  ─────► psmac_ip.c: ARP, ICMP, DHCP, mDNS, TCP
                                                     psmac_remote.c: HTTP/1.0, the endpoints ──► frame buffer (read)
                                                                                             ──► .PiInput's ring (events)
                                                                                             ──► log ring, low memory (read)
                                                                                             ──► the 68k's reset / NMI
```

**Stage 1, the capabilities** (`emu68/src/psmac/psmac_remote.c`), each one a function that the
HTTP layer and a semihosting debug channel (`mac.remotefile=BASE`, the testbed only) both call:

- *screenshot*: the Mac's main screen as PNG, read from the pixels the Mac drew (the window
  region or the big region, docs/10) at whatever depth, through the colour table for 1 to 8 bits,
  so it is the Mac's picture and not the HDMI scan-out. Before the video driver is open (the boot,
  the console) it is the VideoCore buffer instead. On the SE board the card's 512 by 342 one-bit
  frame buffer *is* the SE's built-in screen (psmac_board_se.c mirrors it row by row), so the same
  picture serves both. Since 20 November the SE board can have a second screen on HDMI (docs/10,
  "Two screens on the SE"): `?display=1` photographs that one. The encoder is our own and streams: a band of rows at a time, filter 0,
  deflate with fixed Huffman codes and an LZ77 search over the pixel to the left, the two to the
  left, and the rows above (a Mac desktop is flat colour and dither patterns: a 1920 by 1200
  Finder screen comes out at tens of kilobytes). Each band is its own IDAT chunk, so nothing waits
  for the whole image, and the service loop spends at most a millisecond or so of a turn on it.
  `?scale=2` or `4` averages boxes of pixels.
- *input*: absolute moves, button down and up, click and double-click, keys by Mac virtual key
  code or by character, typing a string. They go into `.PiInput`'s ring, the path every USB and
  Bluetooth keyboard and mouse takes, so the Mac sees ordinary events. Two event types are new in
  the ring: an absolute mouse position (the driver sets RawMouse and MTemp to it, clipped to
  CrsrPin) and a Mac key code (it skips the USB-to-Mac table). Characters are typed as the US
  layout's keys with Shift where needed. A queue in the kernel paces the events (a key's down and
  up 30 ms apart, a click's 80 ms, a double-click inside the Mac's default double-click time), so
  the Mac's 20-event queue never overflows.
- *log*: the kernel's 64 KiB log ring with a byte cursor (`since=N`, the count Aurora68 Log uses), the
  values of secrets hidden (`wifi.psk`, anything whose name ends in psk, pass, key, secret or
  token, `mac.wifisim`'s password, `bt.dev`'s link key), and optionally only the lines of some
  parts (`tags=BOOT,CRASH`).
- *state*: a small JSON document: the board, the screen's size and depth, CurApName ($910), Ticks
  ($16A) and whether they advance, the last crash record and system error since the last restart
  (psmac_crash.c), the network links and the addresses on them, the disk images.
- *control*: restart the Mac (the 68k is sent to the ROM's reset entry, $4080002A, in supervisor
  mode with interrupts masked, the way Restart from the Finder ends; the ROM's first instructions
  switch off the MMU and issue RESET, which resets the card); an NMI (the programmer's switch:
  level 7, the System's debugger prompt or MacsBug); and, last resort, a reboot of the whole Pi
  through the watchdog.

**Stage 2, the transport** (`emu68/src/psmac/psmac_ip.c`). The kernel has no IP stack of its own:
the Mac's TCP/IP (Riptide, MacTCP) runs on the 68k and the kernel moves frames between the chips
and the Mac's rings. The new stack is deliberately small: IPv4 with ARP answers, ICMP echo, a
DHCP client, mDNS (`aurora68-pi.local`), and a TCP that only accepts connections (four at a time,
a 32 KiB send buffer each, go-back-N retransmission, no congestion control: it is for the local
network). It runs on core 3 inside the service loop and never waits. The drivers hand it every
received frame before the Mac's ring (one call: "the kernel's, the Mac's, or nobody's") and take
its frames when the Mac's transmit ring is empty.

**Stage 3, the agent's side** (`tools/aurora68-mcp.py`, `tools/aurora68.py`): an MCP server over
stdio and a plain command line client, both Python 3 with the standard library only, calling the
HTTP API.

## Addresses: wired and WiFi differ

*Wired* (the Pi 3's LAN9514 or LAN7800, the Pi 4's GENET, the testbed's file bridge): the kernel
has a MAC address of its own, the Pi's with the locally administered bit set and the top bit of the
last byte flipped (`remote.mac=` sets another), and gets its own IPv4 address by DHCP. A switch
then sends it its own frames. The chips are told to pass them: the LAN7800 has a second perfect
filter slot for it; the LAN9514 and GENET have one address register, so they go promiscuous while
the remote is on and the kernel filters in software what the Mac would not have received (a
switched network sends a host little that is not its own, so this costs little). The Mac's traffic
does not change: its address, its filter, its frames.

*WiFi* (unit 0, the Broadcom radio): an 802.11 station has one MAC address per association, and
the FullMAC firmware of the 43430 and 43455 offers a second interface only as an access point or
for P2P, not a second station on the same network. So the kernel shares the Mac's MAC address,
and by default its IP address too, taking TCP port 8068 on it: `http://MAC-ADDRESS:8068/`, or
`http://aurora68-pi.local:8068/` (the kernel's mDNS answers with the Mac's address in this mode).
The first real access point it met allowed one IP address per wireless station and never delivered
the ARP request for a second one (docs/08 item 86); many do. With `remote.wifi=own` the kernel
asks instead for an IPv4 address of its own with a different DHCP client identifier (option 61, "aurora68-pi",
which RFC 2131 makes the lease's key) and host name (option 12). Frames are then steered by IP
address: to the kernel's address, the kernel's; broadcasts and multicasts, both; everything else,
the Mac's. Some home routers key leases by the hardware address alone and would hand both the same
address; the kernel sees that (it watches the Mac's own frames for its address) and falls back to
sharing the Mac's address as well, taking only TCP port 8068 to it. The trade-off of the shared
mode: the Mac cannot use port 8068 itself, and the remote is reachable only while the Mac has an
address.

The interface is the wired one while its link is up, else the WiFi (`remote.link=wired|wifi`
fixes one). The HTTP server listens on port 80 and on 8068 of its own address.

## Security

- Off unless `remote=on` is in cmdline.txt. Off, nothing of it runs: no frames are looked at, the
  chips' filters stay as they were, and the service loop's cost is one test.
- A token is required: `remote.token=...` in cmdline.txt (at least 8 characters; without one the
  remote stays off and says so). Every request but the page at `/` must carry it as
  `X-Aurora-Token: ...` or `Authorization: Bearer ...`; it is compared in constant time. The token
  never appears in the kernel's log, the log endpoint or aurora68.log.
- Only the local network: requests from addresses outside RFC 1918 (10/8, 172.16/12, 192.168/16)
  and link-local (169.254/16) are refused with 403 unless `remote.any=1`.
- HTTP, not HTTPS: the token crosses the local network in the clear, like a printer's or a
  router's admin password. The note in "Setting it up" says so.

## Cost

Off: nothing. On and idle: the stack's poll is a timer check per turn of the service loop, the
frames it is shown are a few comparisons each. A screenshot is read from the frame buffer on core
3 while the 68k runs on core 0, a band per turn, so the Mac does not stop; the measurement is in
"What was tested".

## Setting it up

On the Pi's SD card, add to the one line of `cmdline.txt` (a token of your own, 8 characters or
more, letters and digits; keep it out of anything you share):

```
remote=on remote.token=CHOOSE-A-LONG-TOKEN
```

Optional words: `remote.name=NAME` (mDNS name, default `aurora68-pi`), `remote.ip=A.B.C.D/24,ROUTER`
(a fixed address instead of DHCP), `remote.link=wired|wifi`, `remote.wifi=own` (on the WiFi,
an IP address of its own instead of the Mac's port 8068), `remote.mac=02:...` (the wired address), `remote.any=1`
(answer every source address, not only the local network: only behind a firewall you trust).

Finding the Pi: `http://aurora68-pi.local/` from a Mac (mDNS), on the WiFi `http://aurora68-pi.local:8068/`; or the address in the kernel's log
(the HDMI console during the boot, Aurora68 Log, or aurora68.log), a line such as
`[REMOTE] address 192.168.1.23/255.255.255.0 on the wired port (DHCP from 192.168.1.1, lease 86400 s, router 192.168.1.1): http://192.168.1.23/ or http://aurora68-pi.local/`;
or the router's list of clients, where it appears as `aurora68-pi`. When the kernel has to share
the Mac's address on the WiFi the line says `http://ADDRESS:8068/`.

In a browser, `http://aurora68-pi.local/` is a page with the screen: type the token once (the
browser keeps it), click on the picture to click on the Mac, double-click to double-click, and with
the picture focused the keys go to the Mac.

On the MacBook, with `tools/aurora68.py` and `tools/aurora68-mcp.py` copied from the release or the
repository (Python 3 is all they need):

```
export AURORA68_URL=http://aurora68-pi.local AURORA68_TOKEN=CHOOSE-A-LONG-TOKEN
python3 aurora68.py state
python3 aurora68.py screenshot mac.png
python3 aurora68.py click 20 10                     # the Apple menu
python3 aurora68.py log --tags CRASH,BOOT
```

Claude Code:

```
claude mcp add aurora68 --env AURORA68_URL=http://aurora68-pi.local \
    --env AURORA68_TOKEN=CHOOSE-A-LONG-TOKEN -- python3 /path/to/aurora68-mcp.py
```

Claude Desktop: Settings, Developer, Edit Config opens `claude_desktop_config.json`
(`~/Library/Application Support/Claude/` on a Mac); add

```json
{
  "mcpServers": {
    "aurora68": {
      "command": "python3",
      "args": ["/path/to/aurora68-mcp.py"],
      "env": { "AURORA68_URL": "http://aurora68-pi.local", "AURORA68_TOKEN": "CHOOSE-A-LONG-TOKEN" }
    }
  }
}
```

and restart Claude Desktop. The tools are `screenshot` (the main screen, or with `window` one window; `front`
brings it forward first), `list_files`, `read_file`, `write_file`, `open` and `list_apps` (with the Aurora68 Agent
on the Mac, "Files and applications", below),
`list_windows`, `click`, `move`, `drag`, `type`, `key`, `log`, `state` and `restart`.

## The HTTP API

| Request | What |
|---|---|
| `GET /` | the page (no token needed: it holds nothing but asks for the token) |
| `GET /screen.png[?display=0\|1][&scale=2\|4][&src=hdmi]` | a screen, PNG, 24-bit RGB: `display=0` (the default) the first, the SE's on the SE board, `display=1` the HDMI screen when the SE board has two (docs/10); one at a time (503 "another screenshot" while one is sent) |
| `POST /input[?wait=1]` | a JSON event or an array of them (below); `wait=1` answers once they have all been played |
| `GET /log[?since=N][&tags=BOOT,CRASH]` | the log from byte N as text; `X-Log-Next` is the N for next time, `X-Log-Lost` the bytes overwritten before they were read |
| `GET /log?records=1` | the `[BOOT]` and `[CRASH]` lines kept since the Pi started (the first 4 KB of the boot's, the newest 12 KB of crash records), which the 64 KB ring soon loses |
| `GET /windows` | the Mac's windows, front to back, as JSON ("Windows", below) |
| `GET /screenshot?window=ID\|title=TEXT\|screen=N\|rect=l,t,r,b[&content=1][&scale=2]` | a PNG of one window, one screen or a rectangle of the desktop, composed from the screens it is on; `scale` here doubles ("Windows", below) |
| `GET /state` | JSON (below) |
| `POST /restart[?how=mac\|nmi\|pi\|recovery]` | restart the Mac (default), the programmer's interrupt, reboot the Pi, or reboot it into Aurora68 Setup and Recovery (recovery.txt, docs/08 item 106) |
| `GET /files/list`, `/files/info`, `/files/data`, `PUT /files/data`, `POST /files/mkdir`, `/files/delete`, `/files/rename`, `GET /apps`, `POST /apps/open`, `/apps/quit` | files and programs on the Mac, through the Aurora68 Agent ("Files and applications", below) |

Events: `{"type": "move", "x": X, "y": Y}`; `"down"` and `"up"` (x, y optional); `"click"` with
`"count": 2` for a double-click, `"button": "right"` for a Command-click, `"mods": "shift"`;
`"drag"` with `x, y, to_x, to_y`; `"key"` (or `"keydown"`, `"keyup"`) with `"key": "return"`, a
character such as `"q"` or a Mac virtual key code as `"code"`, and `"mods": "cmd"`; `"text"` with
`"text"`; `"wait"` with `"ms"`. Coordinates are global: the main screen's top left is 0,0. With
`"display": N` they are a point of screen N instead, wherever Monitors put it (the kernel adds the
screen's place from QuickDraw's GDevice); `aurora68.py --display 1 click X Y` does that.
Modifiers are `cmd`, `shift`, `option` (`opt`, `alt`) and `control` (`ctrl`).

State: `board` (solo, se, iici), `screen` (width, height, depth, and `source`: `mac` for the Mac's
frame buffer, `console` before the video driver has opened), `mac` (`state`: `running`,
`starting`, `system error` or `stopped` when Ticks have not moved for three seconds; the
`application` in front from CurApName; `ticks`; `ds_err_code`, the system error number the ROM
keeps at $AF0; the last `system_error` and `exception` since the last restart), `remote` (the
link, the address, whether it shares the Mac's, the counters, the longest PNG band in
microseconds), `network` (both units: kind, link, MAC address, the Mac's own IPv4 address on it,
frames), `screens` (each screen of the card: `display`, `what` (se, hdmi or card), its size,
depth and pixel base, and from QuickDraw where it is on the desktop, `left`, `top`, `right`,
`bottom`, `active`, `main`), `power` (the firmware's GET_THROTTLED bits as `throttled`, each "now" bit and each
"since boot" bit by name; `null` before the first reading and under QEMU, docs/12 "Power through
the socket"), `disks`, `log_head`.

In the testbed, `mac.remotefile=BASE` serves the same requests through semihosting (BASE.in and
BASE.out must exist when QEMU starts; `tools/aurora68.py --semihost BASE state`), and
`PINET_KERNEL=PORT python3 tools/pinet.py BASE peer` makes the kernel's HTTP server reachable at
`http://127.0.0.1:PORT/` from the host, through the file bridge (pinet's DHCP gives the kernel
192.168.68.3 and the Mac 192.168.68.2).

## Windows: the list, and a picture of one (21 November 2026)

The user shows results by photographing the Mac's screens, and asked for the Mac's windows as a list
and a screenshot of the one they choose. The kernel reads them out of the Mac's memory
(`emu68/src/psmac/psmac_windows.c`), so nothing runs on the Mac for it and it works under any system.

**Where the windows are.** The Window Manager keeps each window as a WindowRecord in a heap, linked
front to back by `nextWindow` from the low-memory global WindowList ($9D6) (Inside Macintosh I,
"Window Records"). From the record: the title (`titleHandle`, a Pascal string in Mac Roman, given as
UTF-8), `windowKind` (2 a dialog or alert, 8 an application's document, negative a desk accessory,
others the application's own), `visible`, `hilited`, and the bounding boxes of the structure region
(frame, title bar and content, global coordinates) and the content region. Which screens a window is
on comes from the screens' places on the desktop, which QuickDraw's GDevices hold (`gdRect`).

**System 7's layers.** Under System 7 WindowList holds the windows of one application only: the
Window Manager puts each application's windows in a layer of their own, and the Process Manager makes
an application's layer current when it switches to it (System 6's MultiFinder swapped WindowList
itself). Layers are not documented. A RAM dump of System 7.1 with the Finder, PiLog and the Calculator
open (taken through the remote's semihosting channel) showed how they are kept: a layer is itself a
WindowRecord, with one window definition for all layers, its first window in the `windowPic` field
(148) and its parent layer in `dataHandle` (130), 0 for the root; the long at $A90 is the current
layer, whose first window is WindowList; the root holds one layer, which holds the applications'
layers front to back. A layer's `refCon` is the Process Manager's record of its application, which
points back to the layer at $70 and holds at $54 a handle to the application's saved low memory, its
CurApName at $38 and the layer once more at $58. So the kernel climbs from $A90 to the root by the
parent links, walks the tree front to back, takes the current layer's windows from WindowList, and
names each layer's windows by their application: CurApName for the current one, the saved CurApName
for the others, each only when both links back to the layer agree (a desk accessory's name, which
begins with a NUL, without it). If anything does not hold together (System 6, a later system that
keeps layers differently, a moment when the tree is being changed), the list is WindowList alone,
which is always right for the application whose turn it is, and the answer's `X-Aurora-Windows`
header says `WindowList` instead of `layers`.

**Safety.** The 68k goes on running while the kernel reads, and may be closing a window or compacting
a heap. Every pointer is checked to be even and inside the Mac's RAM (as the kernel gave it; MemTop
is no use, System 7's Process Manager leaves it at 2.8 MB on a 16 MB Mac) before it is followed;
handles' master pointers are masked to 24 bits when MMU32Bit ($CB2) says 24-bit mode, where their top
byte holds the lock, purge and resource bits, and a purged handle (NIL) means no title; a region must
be 10 bytes or more, even, and its box ordered; every list has a bound (64 windows a layer, 32 layers,
6 levels, 128 windows in all). Reads are whole aligned longs through the 68k's current MMU tables,
which refuse anything that is not RAM or ROM, so a wrong pointer that passed the checks still cannot
read outside the Mac's memory. The list is read twice, and again up to four times until two readings
agree (`X-Aurora-Windows: ..., consistent`, or `changing while read`). On the host, the reader ran on
the RAM dump with 20,000 random corruptions of the low-memory globals, records, layers, regions and
Process Manager records (pointers bent to other records to make loops, bytes set to $FF): no hang, no
read outside the dump, at most the windows that were still sane.

**The picture.** `/screenshot` takes a rectangle of the desktop in global coordinates and composes it
from the screens' own frame buffers, each at its own depth and through its own colour table, as
`/screen.png` does for a whole screen: a window across the SE's screen and the HDMI screen comes from
both, and what lies under no screen (below the 342-pixel SE screen when the HDMI screen is taller) is
dark grey ($404040, darker than any desktop pattern; the first test used $808080 and the gap could not
be told from the HDMI screen's grey desktop). The rectangle is clipped to the bounding box of the screens, so a window dragged
partly off them is shown where it is on them (`X-Aurora-Clipped-From` gives the rectangle asked for).
It is a crop of the screen: windows in front that overlap it are in the picture, as they are on the
screen (`X-Aurora-Covered-By` lists them); a hidden window is refused (409). `scale=2` (3, 4) doubles
each pixel, for the SE's small one-bit screen; this is the opposite of `/screen.png`'s `scale`, which
shrinks, and is lowered by itself if the picture would be wider than 2560. Rotation (PiDisplay's 90,
180 and 270 degrees) changes nothing here: the frame buffers are the Mac's, in the Mac's coordinates,
and the turning is done on the way to the VideoCore. One picture at a time, as before (503 "another
screenshot"); the encoder is the streaming one of `/screen.png`, a band of rows per turn.

| Request | What |
|---|---|
| `GET /windows` | JSON, an array front to back: `{"id": "0x00fa3090", "title": "Mac Volume", "kind": "application", "window_kind": 20, "visible": true, "hilited": true, "front_index": 0, "rect": [291, 110, 698, 319], "content": [292, 129, 696, 317], "screens": [0], "application": "Finder"}`. `id` is the WindowPtr, the same while the window exists (a window opened later may get the address of one closed); `rect` and `content` are left, top, right, bottom; `application` only where it is known |
| `GET /screenshot?window=ID[&content=1][&scale=2]` | the window by its id (`0x...` or decimal): its structure rectangle, or with `content=1` its content rectangle. A closed window: 404 "no window ... it has been closed" |
| `GET /screenshot?title=TEXT...` | the frontmost visible window whose title contains TEXT (case ignored for ASCII; %-encode it) |
| `GET /screenshot?screen=0\|1[&scale=2]` | one whole screen, as the Mac has it on the desktop |
| `GET /screenshot?rect=l,t,r,b[&scale=2]` | any rectangle of the desktop, global coordinates |
| `GET /screenshot?window=ID\|title=TEXT&front=1[&restore=1]` | the window brought to the front first ("Bringing a window to the front", below); `restore=1` puts back the application (and window) that was in front afterwards |

The headers of a picture: `X-Aurora-Rect` (what was taken, global), `X-Aurora-Scale`, `X-Aurora-Window`,
and when they apply `X-Aurora-Clipped-From` and `X-Aurora-Covered-By`; with `front=1` also `X-Aurora-Front`
(how the window came forward) and `X-Aurora-Covered-By` always, empty when nothing covers it any more.

**Bringing a window to the front** (21 November 2026). On the real SE the owner photographed a window that
was half under another one, and the picture showed the other one, as the screen does. `front=1` brings the
window forward first, the way a person would. Under System 7 the Aurora68 Agent (below) makes the window's
application the front one (SetFrontProcess for the application the window list names); if the window is
then still behind another window of its own application, the kernel clicks once on a visible part of its
title bar: 24 pixels are kept clear at each end for the close, zoom and collapse boxes, the part between
the close box and the title is tried first, and a point is visible when no window in front covers it and a
screen is under it. The pointer goes back where it was (past the window's corner if that was on the window).
Then the kernel waits until the window is first in the list and its update region is empty (the
application has drawn it; `psmac_windows.c` now reads `updateRgn`), at most a second and a half, and takes
the picture. Without the agent, or under System 6, the click alone does it when part of the title bar shows.
Nothing is clicked while a dialog is in front, since an alert or a modal dialog would take the click: 409
"a dialog is in front". A dialog is a Dialog Manager window (windowKind 2) or any front window with no title
bar, such as the message boxes Aurora68 Play draws itself. A window with no title bar showing that the agent cannot bring
forward: 409. `X-Aurora-Front` says `already`, `agent`, `click`, `agent and click`, or `timeout, still
behind` / `timeout, still drawing` when the wait ran out (the picture is taken anyway). With `restore=1` the
previously front application is made front again by the agent after the picture, and when it was the same
application, its previous front window is clicked forward. `aurora68.py screenshot --window T --front
[--restore]`, the MCP screenshot tool's `front` (which always restores), and the page's "bring to front"
checkbox ask for it. Tested on System 7.1 in the testbed (below, "What was tested" of "Files and
applications").

**The page** has a "Windows" panel below the screen: Refresh lists the windows (title, size, screens),
each row has a Grab button that shows the window's picture under the list with a "Save ... .png" link;
"doubled" and "content only" apply to the next grab.

**The command line and the MCP server.** `aurora68.py windows` prints the list as a table;
`aurora68.py screenshot --window "Mac Vol" out.png [--content] [--scale 2]` saves one window (a title's
text, or an id from the table), `--rect l,t,r,b` a rectangle. The MCP server has a `list_windows` tool,
and its `screenshot` tool takes `window` (an id or a title's text) and `content`; with `window`, its
`scale` 2 or 4 enlarges.

**What was tested** (21 November 2026, QEMU and MAME, through the semihosting channel and, on the solo
board, over HTTP from the host through pinet; `docs/img/se-window-across-screens.png`):
- *System 7.1, solo board, 32-bit, 16 MB, the screen in thousands*: the Calculator (DA Handler's
  layer), PiLog (now Aurora68 Log) and two Finder windows listed front to back with their applications
  (`X-Aurora-Windows: layers, consistent`), and again in the new order after the Finder came to the front;
  the Finder's Find dialog (its windowKind is the Finder's own, 20) and a Standard File dialog (2,
  `dialog`); pictures of each, with `content=1` and `scale=2`, of a window covered by two others (the
  header named both), of PiLog dragged half off the right edge (clipped, `X-Aurora-Clipped-From`), and a
  404 for the Find dialog's id after Cancel had closed it. A window of 360 by 204 took 16 ms, the whole
  screen doubled to 1280 by 960 in thousands 368 ms (663 KB), the longest band 2.7 ms of core 3.
- *System 7.1, solo board, 24-bit mode* (`mac.ram=8 mac.xpram=8a:00`, MMU32Bit 0, 256 colours): the same
  list and pictures, over HTTP with the token (`aurora68.py windows`, `screenshot --window`), the MCP
  server's `list_windows` and `screenshot` with `window` (and its error for an id that is not there), and
  the page in headless Chrome: Refresh listed the four windows, Grab showed the Calculator doubled with a
  "Save Calculator.png" link, the Finder's desktop window's Grab greyed out.
- *System 7.1, MAME's SE with two screens* (the SE's 512 by 342 and HDMI 1024 by 768 at (512,0), in
  thousands, then in millions): a Finder window on the SE's screen at `scale=2`; Aurora68 Log and the
  Calculator dragged onto the HDMI screen and taken there in colour; a Finder window dragged across
  both screens and below the SE's 342 rows (`screens: [0, 1]`; the SE's part in one bit, the HDMI part
  in colour, the corner under no screen grey); a window dragged past the HDMI screen's right edge
  (clipped); a closed window's id (404). The SE's screen doubled was 780 KB at first, its 50% grey
  desktop defeating the encoder's four distances; with one and two source pixels back as two more it
  is 30 KB.
- *System 6.0.8, solo board, 24-bit, 8 MB*: with the Finder alone, every window (the disk's, the
  Alarm Clock desk accessory in the Finder's list, the Set Startup dialog as `dialog`, the hidden
  Clipboard window); pictures of them. With MultiFinder (switched on through Set Startup and a restart)
  WindowList holds only the windows of the application whose turn it is, as MultiFinder swaps it with
  the rest of the application's low memory: with the Alarm Clock open the list was the Alarm Clock
  alone, `application` "DA Handler". System 6 has no layers to walk, so under MultiFinder the list is the
  front application's windows only; MultiFinder's own records of the others were not looked into.
- *Not verified*: real hardware; a window on a screen turned by PiDisplay (the code takes the Mac's
  frame buffers, which are in the Mac's coordinates, so nothing should differ); System 7.5 and later,
  whose Process Manager may keep its records differently (the names would then be missing, or the list
  fall back to WindowList); titles outside ASCII on a real Mac (the Mac Roman table was checked against
  Python's on the host). The mouse pointer is part of the picture where it is over the window, because
  QuickDraw draws it into the frame buffer.

## Aurora68 Setup and Recovery shares the transport (22 November 2026)

A recovery start (docs/08 item 106, `psmac_recovery.c`) uses this stack for a page of its own: with
`psmac_recovery_on()` the `remote_tcp_*` calls go to `recovery_tcp_*`, `psmac_ip_init_recovery()` starts the stack
whatever `remote=` says (on the WiFi with an address of its own, and a link-local address when no DHCP server
answers), and the remote stays off, since there is no Mac to drive. The local-network rule is the one above
(`psmac_remote_private_source`). The remote has one new request: `POST /restart?how=recovery`
(`aurora68.py restart recovery`) writes `recovery.txt` on the card and restarts the Pi, which then starts in recovery.

## Files and applications (21 November 2026)

The owner asked for "a command line on the client for jobs like this. Screen shots. Push files etc".
Screenshots, windows and input the kernel does alone; a file or a program needs the Mac's own File Manager
and Process Manager. The kernel could read a disk image's blocks itself, but not while the Mac has the
volume mounted (the File Manager keeps the catalog and the allocation map in its cache) and not at all on a
SCSI disk only the Mac reaches. So a small program on the Mac does these jobs for the kernel: the
**Aurora68 Agent**.

**The agent** (`mac/piagent/agent.c`, `entry.s`; `mac/build/Aurora68-Agent.bin`, an INIT of 22 KB, creator
`PIAG`) goes into System Folder:Extensions under System 7 and the System Folder under System 6 (the layout
helper and the release's "Extensions (to install)" folder put it there). It is in C, built with the project's
m68k GCC kit (`mac/c`, `tools/mac/elf2init.py`) like the Wi-Fi menu. At startup it copies itself into the
system heap, installs a VBL task and hooks the GetNextEvent filter (jGNEFilter), and logs `[MAC] A68 Agent:
installed under System 7.1, the Process Manager, startup volume Mac HD`.
- *When it works.* A synchronous File Manager call at interrupt time can deadlock the File Manager's queue,
  and the Process Manager's calls are not allowed there either. The GetNextEvent filter runs at the end of
  every GetNextEvent and WaitNextEvent of any program, at application time, under System 6 (Finder alone or
  MultiFinder) and System 7 alike; it does nothing while FSBusy ($360) says the File Manager is inside a call
  (a disk-switch dialog's ModalDialog calls GetNextEvent from inside one). The VBL task counts a heartbeat
  the kernel watches and, under System 7, wakes the program in front with WakeUpProcess while a request
  waits, so that a program asleep in WaitNextEvent comes round within a tick (without it each request
  waited for the end of the front program's sleep, 120 to 200 ms in the testbed). Consecutive requests (a
  transfer's chunks) are served in one go for up to five ticks, then the program in front gets its turn.
- *No state between requests.* The filter runs in whichever program's process; System 7 closes a quitting
  program's files. So every request names a full path and the agent opens and closes the file within it;
  the kernel keeps everything else (offsets, the MacBinary header, the HTTP connection). A request is
  carried out once even if the filter is entered again while it runs. A change to the catalog (mkdir,
  delete, rename, the end of an upload) is flushed to the disk at once.
- *The card.* `psmb.h` "the Aurora68 Agent": a 2 KiB block at window offset $5A000 (the request: op, path,
  offsets, type and creator; the answer: status, result, a note) and a 24 KiB transfer buffer at $52000,
  both in the first megabyte so a 24-bit System 6 reaches them ($95A000, $952000). The agent reads and
  writes files straight into the buffer. The kernel writes the block's magic at start-up; an agent on an
  older kernel says so in the log and does not install.
- *Programs* (System 7). `open` launches an application with LaunchApplication, or brings it to the front
  (SetFrontProcess) when it runs already. A document: its application is found by searching the catalog
  for an 'APPL' of the document's creator (PBCatSearch, the document's volume first); not running, it is
  launched with an 'odoc' Apple event as its launch parameters (AECoerceDesc to 'appa'), which the Process
  Manager also turns into the Finder information of the applications of before System 7 (Aurora68 Play
  takes no Apple events and played the file it was launched with); running, it is sent the 'odoc' event,
  which only applications that take Apple events accept (409 "takes no Apple events: quit it, then open the
  document" otherwise). `apps` lists the processes (GetNextProcess, GetProcessInformation), `quit` sends a
  'quit' Apple event (the program may ask to save first; the Finder is never quit). Under System 6: `open`
  launches applications with _Launch (beside the others under MultiFinder, in place of the Finder without
  it, as a double-click would), `apps` is the current application (CurApName), documents and `quit` answer
  501. The launch itself is made after the request has been answered: from inside it LaunchApplication came
  back into the filter while the request was still under way.

**The HTTP API** (each needs the token, like every other request; one file job at a time, a second one gets
503 "another file job", which both tools retry):

| Request | What |
|---|---|
| `GET /files/list?path=P` | a folder's entries as JSON, `{"path", "entries": [...], "count"}`; each entry `name`, `kind` (file, folder, volume), for a file `type`, `creator`, `data` and `rsrc` (the forks' lengths), `created`, `modified` (the Mac's local time), `flags` (the Finder flags), `locked`, `open`, `id`; for a folder `entries`; without `path` the volumes, with `free_kb`, `size_kb`, `startup`. A file's path lists the file. Large folders come in chunks of up to 307 entries (or six ticks of the Mac's time each) |
| `GET /files/info?path=P` | one entry, with `path` |
| `GET /files/data?path=P[&fork=rsrc][&macbinary=1]` | the data fork (or the resource fork) as it is, or with `macbinary=1` a MacBinary II file (both forks, type, creator, Finder flags, dates; the header's CRC). `Content-Length` is known from the start; `X-Aurora-Name`, `-Type`, `-Creator` (%-encoded bytes), `-Flags`, `-Data-Length`, `-Rsrc-Length`, `-Created`, `-Modified`, `-Format` |
| `PUT /files/data?path=P[&macbinary=1][&type=TEXT&creator=ttxt][&force=1][&mkdir=1]` | the body (with `Content-Length`) as the file: MacBinary sets the forks, type, creator, Finder flags and dates, a raw body is the data fork with `type`/`creator` (`????` otherwise). A path ending in `:` is a folder and the MacBinary file's own name is used. An existing file is refused (409) unless `force=1`, which empties both its forks first; missing folders on the way are made only with `mkdir=1`. A new file that does not arrive whole is deleted again |
| `GET /files/progress` | the transfer under way (23 November 2026), asked on a connection of its own beside the one carrying the file: `{"active": true, "job": "push", "path", "phase" ("writing", "reading", "creating the file", "replacing the old file" ...), "fork" ("data", "rsrc"), "done", "total" (the forks' bytes the Mac has written or read, of all of them: a MacBinary header and padding are not counted), "received" (an upload's body as it has come in), "length" (its Content-Length), "sent" (a download's answer as it has gone out), "ms", "requests", "mac_ms", "network_ms"}`; between jobs the last one, `{"active": false, ..., "ok", "last"}`. Never busy, even while a job holds the agent. A kernel before 23 November answers 404 |
| `POST /files/mkdir?path=P[&parents=1]` | a folder (with `parents`, the missing ones above it too, and no error if it is there) |
| `POST /files/delete?path=P` | a file, or a folder that is empty (409 otherwise): never more than one item |
| `POST /files/rename?path=P&to=Q` | a new name, another folder on the same volume, or both; an existing folder as `to` moves the item into it |
| `GET /apps` | JSON: `system`, and `apps` with `name`, `type`, `creator`, `psn`, `front`, `background_only`, `desk_accessory`, `apple_events`, `size`, `free` |
| `POST /apps/open?path=P` | `{"opened": true, "how": "being launched" \| "running already: brought to the front" \| "its application is being launched with it" ...}` |
| `POST /apps/quit?name=N` or `?creator=C` | `{"quit": N, "sent": true}` |

Paths are UTF-8 in the URL (%-encoded) and Mac Roman on the Mac: "Mac HD:Folder:File", and a leading `:`
is the startup volume (":System Folder:Extensions"). Names of more than 31 characters, empty parts ("a::b",
which climbs a folder in the File Manager's syntax) and characters Mac Roman does not have are refused (400).
Errors are JSON with the Mac's reason in words: 404 not found (or a folder on the way, or no such volume),
409 it is there already / busy: the file is open / the folder is not empty / it is a folder, 403 locked or
write-protected, 507 the disk is full, 501 not on this system, 503 when the agent is not there, 504 when it
does not answer within 20 seconds (30 for `open`): "the Mac is busy (a menu held down, a modal dialog, a long
disk operation) or the program in front takes no events". When the agent is not installed (no heartbeat for
5 s, or no magic) a request waits three seconds and answers 503 "the Aurora68 Agent is not running on the
Mac: put it into the System Folder (System 6) or System Folder:Extensions (System 7) and restart". An error in
the middle of a download, after the headers have gone, can only cut the connection short: the client sees
fewer bytes than `Content-Length`, and `/state`'s `agent.last` says why. `/state` has an `agent` member:
`installed`, `alive`, `version`, `system`, `startup`, `process_manager`, `multifinder`, `served`, `chained`,
`repeats`, `wakes`, the job under way, counts, and the last job's outcome; for a transfer, where its time went:
`push :raw300: done, 307200 bytes in 457 ms, 656 KB/s (17 requests to the agent: 162 ms waiting for the Mac to come
round, 292 ms of its work; 0 ms waiting for the network)`. The wait for the Mac ends at the agent's `taken_seq`
(agent version 2; an older agent's whole time counts as its work); `mac.filestrace=1` logs every chunk.

**Streaming.** Nothing waits on core 3. A download asks the agent for the next 24 KiB as soon as the last
chunk has been copied out of the window and the answer's buffer (56 KiB) has room, so the Mac reads the next
chunk while the network sends the last. An upload is flow-controlled: the TCP window the kernel advertises
is now what the job can take (`remote_tcp_room`, `psmac_ip.c`; until today it was always 8 KiB and
everything was handed over), so a large file waits in the sender's socket; a window update goes out as soon
as the job has taken a chunk (the first test of an upload whose last part was 324 bytes stalled until the
30 s idle timeout: the update waited for two segments' worth of room). The semihosting channel streams
uploads the same way.
The upload's buffer is 64 KiB and the window up to 65,535 bytes (23 November 2026; one chunk, 24 KiB, before),
and a chunk goes to the Mac as soon as 24 KiB are in, with what is in hand written anyway after 150 ms without
new bytes. With the window exactly one chunk wide, a Mac (BSD) sender, which sends only full 1460-byte segments
and keeps back a remainder shorter than a segment and than half the largest window it has seen, held the last
1216 bytes of every chunk until its persist timer fired, five seconds at the least: the owner's 222,208-byte
push took 71 s from a MacBook (3.1 KB/s), and 45 s in the testbed once pinet could send that way
(`PINET_KERNEL_BSD`, below); 0.4 s now (docs/08 item 123). The receive queue holds 64 frames (16 before), for
a window's 45 segments back to back.

**The command line** (`tools/aurora68.py`, standard library; `-h` lists every command with a line each):

```
aurora68.py ls                                   the disks
aurora68.py ls ":System Folder:Extensions" [--json]
aurora68.py info ":SimpleText"
aurora68.py pull ":Documents:Letter" [letter.txt] [--text]      the data fork (--text: Mac Roman and CR to UTF-8)
aurora68.py pull ":Applications:MacPaint" --macbinary           MacPaint.bin: both forks, type, creator, dates
aurora68.py push MacPaint.bin ":Applications:"                   MacBinary: its forks, type and creator, its name
aurora68.py push notes.txt ":Documents:" --text                  UTF-8 to Mac Roman, TEXT/ttxt
aurora68.py push song.mp3 ":Aurora68:Aurora68 Play:Music:"       MPG3/PIPY by the extension
aurora68.py push x.bin "Mac HD:New:Deeper:x" --mkdir --force --type BINA --creator ????
aurora68.py mkdir ":Work:2026" -p | rm ":Work:old.txt" | mv ":Work:a" ":Archive:"
aurora68.py open ":Applications:SimpleText" | open ":Aurora68:Aurora68 Play:Music:Minuet in G.mp3"
aurora68.py apps [--json] | quit SimpleText | quit --creator ttxt
```

A raw file's type and creator come from its extension: `.txt` TEXT/ttxt, `.mp3` MPG3/PIPY (Aurora68 Play's
creator), `.aif`/`.aiff` AIFF/PIPY, `.wav` WAVE/PIPY, anything else ????/????; `--type` and `--creator`
override. A file is taken as MacBinary when its header checks (MacBinary II's CRC, the forks' lengths against
the file's size); `--raw` sends it as data, `--macbinary` insists. A pull is written to `NAME.part` and renamed
once it is whole. Like the other commands, when `AURORA68_URL` has no port and port 80 does not answer within
two seconds, the tools try 8068 (the port the kernel keeps when it shares the Mac's address on the WiFi) and
say on stderr which answered.

*Progress* (23 November 2026). `push` and `pull` (and `recovery put` and `get`) show how far a transfer has
got on stderr: on a terminal one line redrawn in place, elsewhere (a log, a script) a plain line every five
seconds, and nothing with `-q`/`--quiet`. The numbers are the Mac's: a push polls `/files/progress` on a second
connection and shows the bytes written on the Mac, with the fork of a MacBinary file and what the Pi has
received; a pull counts the bytes as they arrive, each of them read on the Mac already. For example
```
push Two Forks  [rsrc]  1.5 MB of 2.4 MB  61%  written on the Mac  921.9 KB/s  0:01 left  (the Pi has 1.5 MB)
pull raw3m  1.5 MB of 2.9 MB  51%  received  172.0 KB/s  0:08 left
```
A kernel from before has no `/files/progress`: the line then counts the bytes sent (which run ahead of the Mac
by what the sockets and the Pi hold) and, once they are all sent, "waiting for the Mac" with the time. Through
`--semihost` (one request at a time) a push shows nothing until it ends. `recovery put` shows the bytes on the
card from `/r/state` and "reading it back" while the kernel checks them.

**The MCP server** (`tools/aurora68-mcp.py`) has `list_files` (a folder, or the disks), `read_file` (the data
fork as text, Mac Roman and CR turned into UTF-8 and newlines, or base64; at most `max_bytes`), `write_file`
(text or base64 as the data fork, TEXT/ttxt by default; `overwrite` and `make_folders` as `--force` and
`--mkdir`), `open` and `list_apps`.

**Safety.** The agent does what the token's holder asks, as the Finder would for someone at the Mac, and no
more: `rm` takes one file or an empty folder and never recurses; nothing is overwritten without `force`; no
folder is made without `mkdir`; `mv` stays on one volume; the Finder is not quit; the volume root cannot be
deleted or opened. It changes no system setting and runs no code it is sent (a pushed application runs only
when it is opened). The remote's other rules hold: off unless `remote=on`, the token on every request, the
local network only. Without the remote on, the agent waits and does nothing.

**Limits.** One job at a time. Speed is the Mac's: in the testbed (QEMU's solo board) the owner's 217 KB
MacBinary file goes in 0.3-0.4 s to a card volume (`mac.disk0=`, 550-750 KB/s; 3 MB at 0.9-1.1 MB/s) and comes
back in 0.1-0.2 s (1-3 MB/s); to and from the startup disk behind the emulated SCSI about 100-170 KB/s, nearly
all of it the File Manager and the emulated SCSI (docs/08 item 123 has the table); real hardware has not been
measured since the fix: over WiFi the network's own rate will show in the log line's "waiting for the network".
pinet's `PINET_KERNEL_BSD=PORT` opens a second port whose connections send as macOS does (full segments, the
silly-window rule, a 5 s persist timer), so the testbed meets what the owner's MacBook does. Document opening and `quit` need System 7; under
System 6 the agent cannot see other programs (MultiFinder's are not enumerated). A document's application is
the first 'APPL' of its creator the catalog search finds (the Finder would ask its desktop database: on the
testbed's disk that knew none of the applications, and after the agent had asked it the Finder fell into a
loop of alerts "an error of type -192", so it is not asked). An old application that takes no Apple events
gets a document only when it is launched with it. `quit`, and `open` of a document for a program that runs
already, send an Apple event, which only a program aware of high-level events can send: when the program in
front is not one (Aurora68 Log, for example), the agent first brings the program being quit, or the Finder,
to the front. On the first start of a new disk image the Finder rebuilds its desktop file, and while it does,
quit a program that `open` started only while the Finder is in front (docs/08 item 90). Only the data fork is
read by `read_file`; aliases are not
resolved; paths with two volumes of one name reach the first.

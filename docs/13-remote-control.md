# Remote control: the Mac's screen, mouse and keyboard over the network, from the Pi

*20 November 2026.* The user asked for a way for an agent (Claude Code or Claude Desktop on their
MacBook) to see and drive the Mac directly, so that a problem can be reported and reproduced
without photographs of the screen. The Pi is the Mac's processor and already owns everything
such a remote needs: the frame buffer the Mac draws into (it converts it to HDMI, docs/10), the
mouse and keyboard (it feeds `.PiInput`'s ring, docs/09), the kernel's log (PiLog's ring) and the
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
- *log*: the kernel's 64 KiB log ring with a byte cursor (`since=N`, the count PiLog uses), the
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
for P2P, not a second station on the same network. So the kernel shares the Mac's MAC address and
asks for an IPv4 address of its own with a different DHCP client identifier (option 61, "aurora68-pi",
which RFC 2131 makes the lease's key) and host name (option 12). Frames are then steered by IP
address: to the kernel's address, the kernel's; broadcasts and multicasts, both; everything else,
the Mac's. Some home routers key leases by the hardware address alone and would hand both the same
address; the kernel sees that (it watches the Mac's own frames for its address) and falls back to
sharing the Mac's address as well, taking only TCP port 8068 to it. `remote.wifi=shared` chooses
that from the start. The trade-off: in the shared mode the Mac cannot use port 8068 itself, and
the remote is reachable only while the Mac has an address.

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
(a fixed address instead of DHCP), `remote.link=wired|wifi`, `remote.wifi=shared` (on the WiFi,
share the Mac's IP address from the start), `remote.mac=02:...` (the wired address), `remote.any=1`
(answer every source address, not only the local network: only behind a firewall you trust).

Finding the Pi: `http://aurora68-pi.local/` from a Mac (mDNS); or the address in the kernel's log
(the HDMI console during the boot, PiLog, or aurora68.log), a line such as
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

and restart Claude Desktop. The tools are `screenshot`, `click`, `move`, `drag`, `type`, `key`,
`log`, `state` and `restart`.

## The HTTP API

| Request | What |
|---|---|
| `GET /` | the page (no token needed: it holds nothing but asks for the token) |
| `GET /screen.png[?display=0\|1][&scale=2\|4][&src=hdmi]` | a screen, PNG, 24-bit RGB: `display=0` (the default) the first, the SE's on the SE board, `display=1` the HDMI screen when the SE board has two (docs/10); one at a time (503 "another screenshot" while one is sent) |
| `POST /input[?wait=1]` | a JSON event or an array of them (below); `wait=1` answers once they have all been played |
| `GET /log[?since=N][&tags=BOOT,CRASH]` | the log from byte N as text; `X-Log-Next` is the N for next time, `X-Log-Lost` the bytes overwritten before they were read |
| `GET /log?records=1` | the `[BOOT]` and `[CRASH]` lines kept since the Pi started (the first 4 KB of the boot's, the newest 12 KB of crash records), which the 64 KB ring soon loses |
| `GET /state` | JSON (below) |
| `POST /restart[?how=mac\|nmi\|pi]` | restart the Mac (default), the programmer's interrupt, or reboot the Pi |

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

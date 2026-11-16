# Alice68's programs on the Macintosh: the plan, and the first steps

## Where the programs come from
Alice68 (reference/Alice68, the same author) is the Pi 4 "AGA machine" whose kernel modules
this project reuses on the service core: the network board (`alice_net.c`), Bluetooth, USB.
On the Amiga side it has two programs people actually use:

| Amiga program | What it is | Pages |
|---|---|---|
| **Alice68-Card** (`amiga/card.c`, GadTools, 1300 lines) | The SD card the Pi booted from, managed from Workbench | Card (partitions, free space), Files (browse FAT/exFAT, delete, rename, new folder, pick a hardfile or ROM), Settings (the options of `cmdline.txt` from one shared table, the Kickstart ROM), Text files (`config.txt` line by line, `cmdline.txt` raw), Bluetooth (state, paired devices, scan, pair, passkey, log). Save writes the two files with `.bak` copies; Restart Pi. Everything goes through `alice68hdf.device`'s card service (`hdf/cardsvc.h`: CS_INFO, CS_LIST, CS_READ, CS_WRITE, CS_DELETE, CS_RENAME, CS_MKDIR), whose FAT and exFAT engines live on the Amiga side. |
| **Alice68-Network** (`amiga/netgui/netgui.c`, MUI, 1400 lines; `Alice68-NetworkGT` in GadTools) | The settings and statistics of the network card | About, Status (both units, a minute of traffic); Networks: Ethernet (rate, duplex, autonegotiation, what each side offers, flow control, link downs, time up, MAC) and WiFi (the connection, the networks in range, click to join); Settings: Ethernet (on/off switch, autonegotiation and the speeds to offer or a fixed speed and duplex, flow control) and WiFi (on/off, network, password, country, join and save, disconnect, power saving); Traffic (rates, bytes, packets, drops, errors, a graph). Apply gives the kernel the settings now; Save keeps them (`ENVARC:alice68wifi.prefs`, `alice68net.prefs`, applied by the device at start). |

Both talk to the kernel through the same boards this project already carries in the slot-$9
window, and that is what makes the port tractable: the Mac side needs windows, not protocol.
The Mac already has the plain stand-ins: PiWiFi (scan, join, disconnect, on/off), PiBlue
(Bluetooth), PiStatus (the status page), PiNet and PiPing (tests).

## What a Macintosh version should be
Classic Mac OS has two homes for this kind of thing: a **control panel** (`cdev`, in the
Control Panels folder, a dialog with items the Control Panel desk accessory or Finder
hosts) and an **application**. Alice68-Network's four pages with lists, a graph and text
fields are more than a cdev of the System 6 era comfortably holds, and Alice68-Card's file
browser is an application by nature. So: two applications, built like the existing ones
(68k assembly, no resource compiler, so they run under System 6 and 7 alike), with proper
windows, menus, the List Manager for lists and the Dialog Manager for the fields once the
pages exist, and a small extension that applies the saved settings at boot (the Mac's
answer to `alice68net.device` reading `ENVARC:` at start).

| Macintosh piece | From | Notes |
|---|---|---|
| **PiNetwork** (application) | Alice68-Network | Pages: Status, Ethernet, WiFi, Traffic, Settings. Scan and join as PiWiFi does today; the Ethernet's link, offers and counters from unit 1; the on/off switches (`AN_CMD_ENABLE`); autonegotiation, fixed speed and duplex, flow control (`AN_CMD_ETHCFG`); WiFi power saving and country (`AN_CMD_WIFICFG`). Save writes "PiNetwork Prefs" in the System Folder (Preferences under 7). |
| **PiNetwork extension** (INIT) | `alice68net.device`'s start-up reading of the prefs | Applies the saved settings to the board at boot: the units' on/off, the Ethernet configuration, the WiFi network (so the Mac joins before the Finder, as `wifi.ssid=` in cmdline.txt does on the kernel side). |
| **PiCard** (application) | Alice68-Card | Needs a card service on the Pi side first: the kernel's own FAT reader exists for the container and firmware, a writer and exFAT do not, and on the Mac build the Amiga device's engines are not there. The Settings and Text files pages (cmdline.txt, config.txt) can come first through a small "files on the boot partition" service in the mailbox; the Files browser after that. Bluetooth is PiBlue's, to be folded in. |

Merging the projects: the kernel side is one tree already (this fork carries Alice68's
modules unchanged, with alice_psmac.c as the platform layer). The Amiga programs and the Mac
programs can share their protocol headers (alice_net.h, alice_status.h, the card service's)
and little else: GadTools/MUI and the Toolbox have nothing in common. One repository with
`amiga/` and `mac/` beside the kernel is the natural shape when the Mac side settles.

## The first step: PiNetwork
PiNetwork grows out of PiWiFi (mac/piwifi.s), whose scan/join dialogue works end to end in the
testbed against the simulated radio (docs/09). The order of work:
1. Pages: the window shows one page at a time, the digits 1-5 switch (menus come with the
   Dialog Manager pass). Status: both units in brief. Ethernet: unit 1's link (speed, duplex,
   autonegotiation, advertised and partner modes, flow control, link downs, time up), address
   and counters. WiFi: PiWiFi's page. Traffic: both units' counters since boot and the rates
   over the last second.
2. Settings: the on/off switch per unit (`AN_CMD_ENABLE` with 0/1), Ethernet autonegotiation
   on/off and the fixed speed and duplex (`AN_CMD_ETHCFG`), WiFi country and power saving
   (`AN_CMD_WIFICFG`); a prefs file written with the File Manager; Apply and Save.
3. The extension that applies the prefs at boot.
4. The Dialog Manager pass: real controls, a list for the networks, a graph.
The testbed covers all of it except the Ethernet settings' effect, which the simulated
units accept and ignore; the GENET on a Pi 4 is where they bite.

## Riptide: the user's TCP/IP stack, and whether to port it
Riptide (github.com/farsonic/riptide) is the user's own TCP/IP stack for the Amiga, the one
Alice68's network device was written for. The question was whether to bring it to the Mac too.
The answer for now is no, and the reasons are worth keeping:
- The Mac already has two stacks that the drivers here serve: MacTCP 2.0.6 works end to end
  over the Pi's Ethernet and WiFi (docs/09), and Open Transport (1.1 and later, System 7.1 with
  the OT installer or 7.5) uses the same `.ENET` driver through its own Ethernet link. Every
  Macintosh Internet program of the period was written for MacTCP's `.IPP` driver interface
  or Open Transport's, not for a stack API of its own; a third stack would have no clients
  unless it wore one of those two interfaces.
- Wearing the MacTCP interface is possible and is the only form in which a Riptide port would
  pay: `.IPP` is a driver with some forty Control calls (TCPCreate, TCPActiveOpen, TCPSend,
  TCPRcv, TCPClose, the UDP set, ipctlGetAddr, the DNR glue), each with asynchronous completion
  through parameter blocks, and MacTCP's own engine is slow 1989 code. A Riptide engine behind
  that interface would be the "faster MacTCP" some people shipped as replacements in the 90s,
  with DHCP where MacTCP has BOOTP. That is a project of weeks, after the drivers it would sit
  on are proven on hardware, and only if MacTCP's speed turns out to matter on the SE.
- What is worth doing sooner and is much smaller: a DHCP client in the kernel or in the
  `.ENET` driver's Ethernet link so MacTCP's "Server" setting gets a real lease, and the Open
  Transport test, since OT is what System 7.5 and later use.

<!-- SPDX-License-Identifier: MPL-2.0 -->
# Bluetooth beside WiFi on the Pi 3B: how Raspberry Pi OS does it, and how Aurora68 does

Written 24-25 November 2026, after the owner's report on the SE: "Bluetooth still causing issues". With the 0.16
kernel (docs/08 item 140) the WiFi had got its address with Bluetooth on. Then the owner paired a Bluetooth LE keyboard
again from the Mac's Bluetooth menu. The WiFi stopped, and then the whole machine locked up: no pointer, no network.
The owner asked: "do some research with how this is done in something like Raspbian". This document records what
Raspberry Pi OS (Raspbian until 2020) does on a Pi 3B, point by point, each with its source. Beside each point it says
what this kernel did, and what it does after the changes in docs/08 item 141.

Sources were read on 24 November 2026: Raspberry Pi's kernel tree at branch `rpi-6.6.y` (github.com/raspberrypi/linux,
the kernel of Raspberry Pi OS Bookworm), RPi-Distro's `pi-bluetooth` (master) and `firmware-nonfree` (the commit
`tools/get-firmware.sh` pins), BlueZ's `src/main.conf` (master), and the GitHub issues named below. No binaries were
downloaded into the repository. Line numbers for our code are those on branch `bt-rpios`.

## 1. The chip and the UART

**Which UART, which pins, flow control.** The Pi 3B's BCM43438 (Cypress CYW43438) puts its Bluetooth half on the
SoC's PL011 (UART0). Raspberry Pi's device tree (`arch/arm/boot/dts/broadcom/bcm2710-rpi-3-b.dts`) gives UART0 the
pins `uart0_pins: brcm,pins = <32 33>` (TXD, RXD, ALT3) and `bt_pins: brcm,pins = <43>` (ALT0, GPCLK2). There are no
CTS or RTS pins. Raspberry Pi's engineer Phil Elwell said why on 5 June 2019, in raspberrypi/firmware#1150 ("Bluetooth:
hci0: Frame reassembly failed (-84)"): "The 3B doesn't have the flow control signals hooked up to the BT modem - we
were short of pins - so it is possible for characters to get lost under load (especially interrupt load)". The 3B+,
the Pi 4 and the two Zeros wire CTS and RTS on GPIO 30 and 31. `pi-bluetooth`'s `btuart` script tells the two apart by
counting the cells of `uart0_pins` (16 bytes means four pins: `bcm43xx 3000000 flow`, otherwise `921600 noflow`).
*Ours:* the same split. `bt_pi.c:166` turns flow control on only for the Pi 4 family and the Zeros. On a 3B only GPIO
32 and 33 are switched (`bt_pi.c:171`), and the PL011 runs with `CR_NOFLOW`. This was learned the hard way on 18
November (docs/08 item 37): with CTS flow control on and nothing driving CTS, the first HCI_Reset never left. GPIO 43
is the one pin we leave alone. Raspberry Pi's tree puts it in ALT0 (GPCLK2) as part of UART0's pin group, and mainline
Linux's `bcm2837-rpi-3-b.dts` names it `gpclk2_gpio43`. No source we found says what the clock feeds. The kernel now
logs GPIO 43's function once Bluetooth is ready (`bt_pi.c`, "GPIO 43 is ..."), so that the owner's log shows whether
the firmware left it as Raspberry Pi OS expects.

**Who loads the firmware.** Raspberry Pi OS first used a user-space `hciattach` (`btuart`, run by `hciuart.service`).
Today the kernel does it: `bcm271x-rpi-bt.dtsi` declares `bluetooth { compatible = "brcm,bcm43438-bt"; max-speed =
<3000000>; shutdown-gpios = <&expgpio 0 GPIO_ACTIVE_HIGH>; ... status = "okay"; }` under UART0. The 3B's tree lowers
that to `&bt { max-speed = <921600>; }`. `btuart` now exits when the kernel has done the work ("Bail out if the kernel
is managing the Bluetooth modem initialisation"). The overlay README lists `krnbt` ("Set to 'off' to disable autoprobing
of Bluetooth driver without need of hciattach/btattach", default "on") and `krnbt_baudrate`. The driver is
`hci_uart`/`hci_bcm` over serdev, with `btbcm` for the Broadcom steps. BT_ON is the firmware's GPIO expander line 0,
WL_ON is line 1 (the WiFi's `mmc-pwrseq`). *Ours:* BT_ON goes through the same expander line, by the firmware mailbox
(`bt_pi.c` `bt_on`). The firmware is `BCM43430A1.hcd` from RPi-Distro's `bluez-firmware` (`get-firmware.sh`), the
name `btbcm.c` gives subversion 0x2209.

**Speeds.** `hci_bcm.c` (`bcm_open`, then `bcm_setup`) starts at the chip's 115200. Because `oper_speed` is set (the
device tree's `max-speed`), "ldisc/serdev will set the baudrate before calling setup()". So the `.hcd` is sent at the
operating speed: Update UART Baud Rate (vendor command 0xfc18), the host's UART follows, the download runs. After
Launch_RAM the chip is back at 115200, and the host follows it (`host_set_baudrate(hu, init_speed)`). Then 0xfc18
moves both to the operating speed again for good. On the 3B both speeds are 921600 with no flow control. `btuart`
(the older way) did the same: `hciattach /dev/serial1 bcm43xx 921600 noflow - $BDADDR`. With the mini UART
(`miniuart-bt`) it was 460800 for `btuart`, and the device tree now says 230400. *Ours:* the firmware at 921600 and
then back to 115200 since 23 November (docs/08 item 136; `bthost.c:461` and `:1244`, `bt_pi.c:276`). After that we
**stay at 115200**, deliberately. Linux reads the UART from its interrupt. This kernel polls it from core 3's loop,
and at 921600 the PL011's 16-byte FIFO lasts 174 µs, which no turn of that loop can promise. At 115200 it lasts
1.4 ms. Even that was not always met (section 4).

**HCI commands at set-up.** `btbcm_initialize` sends HCI_Reset (and waits 100 ms), Read Local Version, Read Verbose
Config (0xfc79), Read Controller Features (0xfc6e) and Read Local Name. `btbcm_patchram` sends Download Minidriver
(0xfc2e, then 50 ms), the records, and waits 250 ms after Launch_RAM. `btbcm_finalize` runs `btbcm_initialize` again
and `btbcm_check_bdaddr`. The kernel's generic `hci_sync` set-up follows: features, buffer sizes, BD_ADDR, class,
name, event masks, SSP mode, inquiry mode, LE Host Supported, LE buffer size, LE event mask. *Ours:* the same in
substance (`bthost.c` `setup_next`, from `:393`).
- **Sleep mode (0xfc27, "Set Sleep Mode"):** `hci_bcm.c` sends it only from `bcm_setup_sleep`, which runs only when
  `bcm_request_irq` finds a host-wake interrupt (`if (bdev->irq <= 0) return -EOPNOTSUPP`). The Pi's device tree has
  no host-wake or device-wake GPIO, so Raspberry Pi OS never sends it, and the chip keeps its default (no UART sleep).
  *Ours:* never sent either. Nothing to change.
- **PCM/SCO routing (0xfc1c):** `pi-bluetooth`'s `bthelper` sends `hcitool cmd 0x3f 0x1c 0x01 0x02 0x00 0x01 0x01`
  ("Route SCO packets to the HCI interface (enables HFP/HSP)"). That is for headsets. *Ours:* no audio, not needed.
- **Coexistence commands:** neither the kernel nor `pi-bluetooth` sends any Bluetooth-side coexistence command. The
  coexistence lives in the WiFi firmware (section 2).
- **The BD address:** `btbcm.c` knows that "The address AA:AA:AA:AA:AA:AA indicates a BCM43430A1 controller with no
  configured address" and sets `HCI_QUIRK_INVALID_BDADDR`. `hci_bcm` then takes the device tree's `local-bd-address`,
  which the Pi firmware fills in. The older `bthelper` wrote one itself (Write BD ADDR, 0xfc01), made from the board's
  serial number. *Ours:* we use what the chip reports. Changing it now would make every existing bond invalid (each
  keyboard knows the Pi by its address), so the kernel only says so in the log when the address is that placeholder
  (`bt_pi.c:882`).

**Order of the radios.** Raspberry Pi OS has none. brcmfmac (SDIO, WL_ON) and hci_uart (UART, BT_ON) probe on their
own, and both load their firmware in parallel. *Ours:* the same. Both start on core 3 when the board exists
(docs/08 item 136).

## 2. The WiFi side of coexistence

**The NVRAM.** RPi-Distro's `brcmfmac43430-sdio.txt` ends with "# Improved Bluetooth coexistence parameters from
Cypress", `btc_mode=1`, `btc_params8=0x4e20`, `btc_params1=0x7530`, and earlier `muxenab=0x1` ("0x1 for UART enable").
The same three lines went into the Zero 2 W's files on 11 January 2023 (commit 8e349de). They came from
RPi-Distro/firmware-nonfree#33 ("Raspberry Pi Zero 2 W - Stuttering sound over Bluetooth", December 2022). There the
engineer wrote that "the improvement comes at the cost of a large reduction in throughput while there is Bluetooth
traffic" and that "there are always going to be limitations when they share the same antenna". *Ours:* the same file,
passed as it is. The WiFi start logs `btc_mode` (docs/08 item 140; `alice_net.c:1387`).

**brcmfmac's btcoex.c.** It writes registers 66, 41 and 68 (`BRCMF_BT_DHCP_REG66 0x2710` and so on) to give the WiFi
priority while DHCP runs. Only `brcmf_cfg80211_crit_proto_start` turns that on, when user space asks for
`NL80211_CRIT_PROTO_DHCP` through nl80211. Raspberry Pi OS's network stack does not ask for it as far as we found.
(Item 140 said Android's `BTCOEXMODE`. The mechanism in this tree is nl80211's critical protocol, and the conclusion
is the same.) *Ours:* `bt_hush` (docs/08 item 140) does the same job from the Bluetooth side: no scans for 10 s around
the WiFi's join and its DHCP.

**WiFi power save.** brcmfmac starts with power save on (`cfg->pwr_save = true`, so `PM_FAST`, `cfg80211.c:7055`). Its
coexistence uses power save to ask the access point to hold frames while Bluetooth has the antenna. raspberrypi/linux
#1444 (below) has a 2021 report that turning power save off made Bluetooth connections fail instead. *Ours:* power
save off unless the Mac asks (`alice_net.c:1407`, `AN_WC_POWERSAVE`), and the owner's WiFi works that way. We note
the difference and leave it alone, because the owner's SE confirmed the present setting.

## 3. How much radio time Bluetooth takes

| | Raspberry Pi OS (Linux 6.6, BlueZ defaults) | Aurora68 before | Aurora68 now (item 141) |
|---|---|---|---|
| LE connection interval asked | 30-50 ms (`hci_core.c`: `le_conn_min_interval = 0x0018`, `le_conn_max_interval = 0x0028`), latency 0 | **7.5-15 ms** | 30-50 ms (`btle.c:191`, `bthost.h` `BT_LE_CI_*`) |
| LE supervision timeout | 420 ms (`le_supv_timeout = 0x002a`) | 5 s | 5 s, kept: a link starved by the WiFi's share is held |
| A device's Connection Parameter Update Request | checked (`hci_check_conn_params`: min ≥ 6, max ≤ 3200, timeout 10..3200 and over two intervals, latency ≤ 499 and what the timeout covers), refused when invalid, else passed to the controller as asked (`l2cap_conn_param_update_req`) | accepted unchecked, passed as asked | checked the same way and refused when invalid (`btle.c:949`); the lower end raised to 15 ms where the device's range allows (`btle.c:959`) |
| LE Remote Connection Parameter Request event (LL procedure) | enabled when the controller has the feature (`hci_sync.c`, event mask bit 5) | masked (`bthost.c:410`), so the controller rejects the LL procedure and the device falls back to L2CAP | unchanged |
| LE scan while connecting to one device | 60 ms window every 60 ms (`le_scan_int_connect`/`window_connect`), the whole radio | 30 ms every 60 ms | 30 ms every 120 ms (`btle.c:184`) |
| Background scan for bonded LE devices away | continuous passive scan 30 ms every 60 ms (`le_scan_interval 0x0060`, `le_scan_window 0x0030`), with no accept list when a device has an IRK and the controller has no LL privacy ("Accept list can not be used with RPAs", `hci_sync.c`) | fast/slow/idle tiers (item 140) | unchanged (`btle.c:170-171`) |
| Discovery ("scan") | interleaved: 5.12 s LE active scan at 11.25/11.25 ms, then 5.12 s inquiry (`DISCOV_LE_SCAN_INT 0x12`, `DISCOV_INTERLEAVED_TIMEOUT 5120`, `DISCOV_BREDR_INQUIRY_LEN 0x08`) | inquiry 10.24 s + LE active 30 ms every 60 ms together | inquiry + LE active 30 ms every 120 ms (`bthost.c:1313`) |
| Page scan | 11.25 ms every 1.28 s, standard (`def_page_scan_int 0x0800`, `window 0x0012`); "fast connectable" (160 ms, interlaced, `hci_write_fast_connectable_sync`) only when asked (BlueZ `FastConnectable = false`) | fast for a minute, then standard, only while a Classic device is away (item 140) | unchanged (`bthost.c:339`) |
| Command timeout | 2 s (`HCI_CMD_TIMEOUT`): `hci_cmd_timeout` logs "command 0x%4.4x tx timeout" and sets `cmd_cnt` back to 1 | none: one lost answer stopped every later command | 2 s, as Linux (`bthost.c:1254`) |
| ACL buffers of a link that goes | given back (`hci_conn_del`: `hdev->le_cnt += conn->sent`), completed packets of an unknown handle skipped, the count never above the controller's | lost; completed packets of an unknown handle counted | as Linux (`bthost.c:181`, Number Of Completed Packets) |

BlueZ's `main.conf` leaves every LE and BR/EDR timing commented out ("If a value isn't provided, the kernel will be
initialized to it's default value"). The kernel's defaults above are therefore what a Raspberry Pi OS machine uses. We
found nothing in RPi-Distro's packaging that changes them.

## 4. Known Pi 3B problems with Bluetooth beside WiFi

- **raspberrypi/linux#1444, "Bluetooth LE connection attempt disables Wifi on RPi3"** (opened 28 April 2016, still open).
  During a UDP stream an `hcitool lecc` to a device that was not there dropped the WiFi to almost nothing until the
  attempt was cancelled. On 8 January 2018 Phil Elwell reported Cypress's advice: hcitool's scan window and interval
  (4 and 4, the whole radio) left no time for WiFi, so he published an hcitool with 64 and 8 (40 ms and 5 ms), calling
  less Bluetooth performance the "least worst compromise". A Pi 3B report in May 2021 says any Bluetooth activity
  drops WiFi packets. This is the owner's first symptom: pairing is an LE connection attempt (60/30 here, 50 %, for up
  to 15 s), after a scan that held the radio for 12 s, and then a link at 7.5 ms.
- **raspberrypi/linux#3727, "Raspberry as CTS-Flooder"** (July 2020): with Bluetooth scanning, the WiFi firmware sends
  CTS-to-self frames about every 3.75 ms to keep the channel quiet for Bluetooth. Cypress answered that this "cannot be
  modified or disabled". Bluetooth scanning costs the whole 2.4 GHz channel, not only the Pi's own WiFi.
- **raspberrypi/firmware#1150, "Frame reassembly failed (-84)"** (opened 5 June 2019, Pi 3B and 3B+): Bluetooth stops
  after a while because bytes are lost on the UART without flow control. Lowering the speed in `btuart` was offered as
  a workaround. The "real fix" of 11 July 2019 was raspberrypi/linux 9bf5cd2, "tty: amba-pl011: Make TX optimisation
  conditional". The PL011 driver had been writing half a FIFO without looking, and on SMP that lost transmitted bytes,
  so commands to the chip vanished. In October 2022 the advice was `dtparam=krnbt` and 460800 baud. Package commits in
  October 2026 set the Pi 3B's Bluetooth to 460800.
- **The usual remedies** (overlay README): `dtoverlay=disable-bt` ("Disable onboard Bluetooth ... restores
  UART0/ttyAMA0 over GPIOs 14 & 15"), `dtoverlay=miniuart-bt` ("uses a lower baudrate, and should only be used with
  low-bandwidth peripherals"), a USB Bluetooth adapter, a 5 GHz network (not on a 3B), or Ethernet. `bt=off` is our
  `disable-bt`.
- **Lock-ups of the whole Pi tied to pairing:** none found. What the reports describe is Bluetooth going dead (#1150)
  and the WiFi going quiet (#1444). One commenter in #1150 suspected the power supply, which is worth remembering on an
  SE (section 5).

## 5. What this meant for Aurora68 (24-25 November 2026)

The owner's sequence was: Look for Devices (inquiry plus an LE scan, 12 s), pair (a direct LE connection attempt at
50 % for up to 15 s), then a new link at 7.5-15 ms with SMP and GATT discovery. Every part of that took more of the
radio than Raspberry Pi OS takes, and #1444 shows the same WiFi stall on Raspberry Pi OS from the connection attempt
alone. So the WiFi stopping is explained, and item 141 cuts each part down.

The lock-up was looked for on both sides:
- *Lost bytes on the UART.* With no flow control and a polled 16-byte FIFO, one turn of core 3's loop longer than
  1.4 ms loses the chip's bytes. In the testbed the heartbeat shows parts of a turn of 2 to 12 ms, and some of 100 ms.
  A pairing also writes `pairings.txt` to the card, in up to three synchronous sector writes (`psmac_card.c`
  `pairings_write`), each waiting for the card's programming time, up to a second. That happens just as the keyboard's
  GATT discovery sends its Report Map. Before item 141, three things followed from a lost byte. The H4 receiver could
  take garbage for a header and swallow the real packet behind it. A lost Command Complete left the host with no
  command credit for ever (no timeout), so no command left again: LE Create Connection, Disconnect, encryption. A lost
  or false Number Of Completed Packets, or a link that went with packets in flight (a keyboard put into pairing mode
  while the host talked to it), shrank the ACL pool until nothing more was sent. Each of these leaves Bluetooth dead
  while the Mac waits in the pairing dialog. They do not, by themselves, stop the Pi.
- *Spinning with a lock held.* We read the Bluetooth host's loops: the H4 receiver, the advertising and EIR parsers,
  the Report Map parser, GATT's continuation requests, SMP. Each one ends on its own input or on a timer. `aes_busy`
  is a flag, never waited on. One fault was found: an LE Rand answer that came after its link had gone could be taken
  for the next link's LE Encrypt, which made the pairing fail with "confirm value failed". The answer is now checked
  against the command that is out. The console lock (`con_lock`) is only held around bounded UART writes. The mailbox
  lock (`vc_mbox_lock`) is taken by Bluetooth only for BT_ON at start, and never across a yield (docs/08 item 79). The
  GPIO function registers are shared under `psmac_gpfsel_lock`, read-modify-write. The SDIO waits yield after 200 µs.
  Nothing found here can stop core 3 for good.
- *The Mac.* The pairing dialog is `ModalDialog` inside Menu Extras' jGNEFilter, guarded by `busy` against
  re-entry, the same pattern as the WiFi password dialog, which works on the owner's SE. It closed only when the Pi
  said the pairing was over. The Pi's host ends any pairing within 30 s (SMP) or 15 s (the connection) by its timers,
  but a host that was dead (above) or restarted left the dialog to Cancel. If the keyboard being paired was also the
  pointer, with no other mouse, nothing could click Cancel: no pointer, while WiFi was off the air. That looks like a
  lock-up. The dialog now also closes when Bluetooth is not ready and after two minutes.
- *Power.* The owner had under-voltage warnings until the USB keyboard was unplugged (docs/08 item 140). Inquiry and
  paging at full power, an LE link, the WiFi and a card write all at once are the Pi's heaviest moment. A brown-out
  stops everything at once. Only the owner's Pi can show this: Aurora68 Status or the `[PSMAC] power:` lines around the
  moment.

What only the owner's Pi can confirm, and what to look for, is in docs/08 item 141.

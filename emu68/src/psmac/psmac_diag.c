/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_diag.c - Setup and Recovery's options and diagnostics, in portable C.
 *
 * Backstory (23 November 2026, docs/08 item 122). The owner asked: "do we have all the debug options in there for
 * Mac, Mac Plus etc. Can we trigger a debug run directly in there for this and other platforms? With a report in
 * the Web GUI?" The recovery page's Options tab knew sixteen options, as a list in the page's script with a second
 * list of allowed values in psmac_recovery.c, and a third list (the page's OB map) of the boards that use some of
 * them; the kernel reads more than a hundred. This file is now the one list: every option a user or a tester may
 * set, grouped (Machine, Display, Sound, Storage, Network, Video card, Debug), with the boards that use it, the
 * values the kernel takes and a line of plain English. The page draws its Options tab from it (GET /r/options
 * sends it), the validator is pdg_value_ok, the one-shot debug start (debug-once.txt) takes only the options
 * marked PDG_F_ONCE, and the board filter is pdg_uses. Options that only make sense in the testbed (mac.remotefile,
 * mac.wifisim, mac.switchsim, mac.hdmisim, mac.resetsim and the rest: README's table marks them "testbed only")
 * are not here: the page does not offer them, and an edit that names one is written as it is, like any word the
 * page does not know.
 *
 * Values are alternatives joined by '|': a plain word (matched without regard to case), or a pattern in which '#'
 * is one or more decimal digits, '%' one or more hex digits (after an optional 0x), '*' any text without quotes or
 * control characters (it must come last), and every other character itself. "#x#" is a screen size, "%,#" an
 * address and a length. The descriptions say what the kernel does with each; README's options table is the long
 * form.
 */
#include "psmac_diag.h"
#include "psmac_cmdline.h"

#define C PDG_COMPACT
#define PF PDG_PLUSFAM
#define A PDG_ALL
#define BUS (PDG_COMPACT | PDG_IICI)                  /* a bus behind the Pi: the PiStorm's GPIO or the testbed's bridge */

#ifdef PSMAC_PC
const char *const pdg_group_name[PDG_GROUPS] = { "Machine", "Display", "Sound", "Storage", "Network", "Video card", "PC card", "Debug" };
#else
const char *const pdg_group_name[PDG_GROUPS] = { "Machine", "Display", "Sound", "Storage", "Network", "Video card", "Debug" };
#endif

const struct pdg_opt pdg_opts[] = {
    /* ---- Machine ---- */
    { "mac.board", PDG_G_MACHINE, 0, A, "se|plus|512ke|512k|128k|solo", 0,
      "The logic board the Pi sits in: se, plus, 512ke, 512k or 128k; solo for a Pi on the bench with no Mac. Not given: the testbed's IIci." },
    { "mac.ram", PDG_G_MACHINE, 0, A, "#", "128", "The Macintosh's memory in MB (the Pi's own; default up to 128)." },
    { "mac.seram", PDG_G_MACHINE, 0, PDG_SE | PDG_PLUS, "1|2|2.5|4", 0,
      "The compact Mac's own memory in MB, where its screen and sound buffers are (at its top). Default: what the board has, else 4." },
    { "mac.fpu", PDG_G_MACHINE, 0, A, "exact|on|double|none", 0,
      "The Mac's FPU: exact, a 68882 in extended precision (the default; on is the same); double, faster, in double precision; none, no FPU, as a real compact Mac." },
    { "mac.sane", PDG_G_MACHINE, 0, A, "rom|se", 0, "Which SANE (floating point): the IIci ROM's own (rom), or the SE ROM's software SANE (se)." },
    { "mac.serom", PDG_G_MACHINE, 0, PDG_PLUSFAM | PDG_SOLO | PDG_IICI, "*", "MacSE.rom",
      "An SE ROM image in the card's root to take exact SANE from (the SE board reads its own ROM)." },
    { "mac.bootcount", PDG_G_MACHINE, 0, A, "#|off", "3", "Starts in a row without the Finder after which the next start is Setup and Recovery; off for never." },
    { "mac.recovery", PDG_G_MACHINE, 0, A, "1", 0, "1: start in Setup and Recovery (Restart normally takes it out again)." },
    { "mac.power", PDG_G_MACHINE, 0, A, "off", 0, "off: stop watching the Pi's power (the log's under-voltage and throttling lines)." },
    { "mac.sensors", PDG_G_MACHINE, 0, A, "off", 0, "off: the status page (Aurora68 Status's gauges) does not ask the firmware for the temperature and the ARM clock." },
    { "mac.altcmd", PDG_G_MACHINE, 0, A, "1", 0, "1: a USB or Bluetooth PC keyboard's Alt and Windows keys swapped, so the key beside the space bar is Command." },
    { "usb", PDG_G_MACHINE, 0, A, "off|pio", 0, "The Pi's USB: off leaves it alone; pio moves data without DMA, for a device that misbehaves." },
    { "mac.pluskbd", PDG_G_MACHINE, 0, PF, "off|on", 0, "off: the Plus's (128K's, 512K's) own keyboard is not read, only USB and Bluetooth ones." },
    { "mac.plusmouse", PDG_G_MACHINE, 0, PF, "#", "2000", "The Plus mouse's lines read this many times a second (0: the mouse off)." },
    { "mac.plusdebounce", PDG_G_MACHINE, 0, PF, "0|1", 0, "0: the Plus mouse's lines taken without debouncing." },
    { "mac.plusjitter", PDG_G_MACHINE, 0, PF, "0|1", 0, "0: the Plus mouse's reads at a fixed rate, without jitter." },
    { "mac.sepram", PDG_G_MACHINE, 0, PDG_SE, "chip", 0,
      "chip: when the SE's clock chip does not answer, leave it so (by default the kernel answers for it from pram.bin)." },
    { "mac.appletalk", PDG_G_MACHINE, 0, PDG_SOLO, "1", 0, "1: AppleTalk active from the start (the solo board's parameter RAM)." },

    /* ---- Display ---- */
    { "mac.screen", PDG_G_DISPLAY, 0, A, "#x#", "1024x768", "The HDMI screen's size in pixels; needed only to differ from the firmware's HDMI mode." },
    { "mac.console", PDG_G_DISPLAY, PDG_F_ONCE, A, "splash|boot|hdmi", 0,
      "What the HDMI screen shows before the Mac's picture: splash, the Aurora68 logo and what the Pi is doing (the default); boot, the kernel's log until the Mac's screen comes up; hdmi, the log for the whole run." },
    { "mac.hdmi", PDG_G_DISPLAY, 0, C, "screen|mirror|log", 0,
      "What HDMI is beside the compact Mac's screen: a second screen of its own (screen, the default), the Mac's picture (mirror), or the log." },
    { "mac.vcdepth", PDG_G_DISPLAY, 0, C, "16|32", 0, "32: ask the firmware for a 32-bit HDMI frame buffer for the second screen (16 keeps the firmware's own)." },
    { "mac.vcswap", PDG_G_DISPLAY, 0, A, "0|1", 0, "1: red and blue swapped on the HDMI screen, should the firmware say the wrong order (0: not)." },
    { "mac.displaywait", PDG_G_DISPLAY, 0, A, "#", "300", "Seconds within which a new HDMI mode must be kept on the Mac, or the old one comes back (0: wait for ever)." },
    { "mac.hdmiclock", PDG_G_DISPLAY, 0, A, "#", "162", "The pixel clock in MHz Aurora68 Display judges the monitor's modes against (162 for a Pi 3)." },
    { "mac.edgejump", PDG_G_DISPLAY, 0, A, "on|off", 0, "off: the pointer goes from one screen to the next only where the two touch (on, the default: anywhere along that side, at the same proportion)." },
    { "mac.screencheck", PDG_G_DISPLAY, PDG_F_ONCE, C, "on|blind|off", 0,
      "The compact Mac's screen: on, read back and repaired; blind, written again without reading (the Plus-era default); off." },

    /* ---- Sound ---- */
    { "mac.audio", PDG_G_SOUND, 0, A, "hdmi|jack|off", 0, "Where the Pi plays the Mac's sound: the HDMI monitor (the default), the 3.5 mm jack, or nowhere." },
    { "mac.audiorate", PDG_G_SOUND, 0, A, "48000|44100", 0, "The output's sample rate." },
    { "mac.sndcard", PDG_G_SOUND, 0, A, "off|on", 0, "off: no Aurora68 Sound card (16-bit stereo); the Sound Manager stays on the ASC." },
    { "mac.sesound", PDG_G_SOUND, 0, C, "off|on", 0, "off: leave the compact Mac's own speaker alone." },
    { "mac.sestream", PDG_G_SOUND, 0, C, "on|off", 0, "on: Aurora68 Play's music on the compact Mac's speaker too (off by default)." },
    { "mac.audiotest", PDG_G_SOUND, PDG_F_ONCE, A, "#|card", "3",
      "A test sound at start: N seconds of 440 Hz left and 660 Hz right; card, the Aurora68 Sound extension's test through the card." },

    /* ---- Storage ---- */
    { "mac.boot", PDG_G_STORAGE, 0, PDG_SE | PDG_PLUS, "auto|image|scsi", 0,
      "The startup disk: auto (SCSI as parameter RAM says, then the card's image), image (the card's image first), or scsi." },
    { "mac.card", PDG_G_STORAGE, 0, A, "off|ro", 0, "The Pi's own SD card: ro reads only (nothing saved), off leaves it alone." },
    { "mac.container", PDG_G_STORAGE, 0, A, "*", "aurora68.cpio", "The container's name on the card, when it is not aurora68.cpio." },
    { "mac.image", PDG_G_STORAGE, 0, A, "*", "System.hda,Apps.img", "The disk images on the card, by name (comma between them); off for none." },
    { "mac.disks", PDG_G_STORAGE, 0, PDG_SOLO, "scsi", 0, "scsi: every disk image a SCSI disk (by default the second and later are PiDisk volumes)." },
    { "mac.usbdisk", PDG_G_STORAGE, 0, A, "on|all|off", 0, "USB flash drives: on, the HFS ones appear on the desktop; all, every stick; off, none." },
    { "mac.usbview", PDG_G_STORAGE, 0, A, "off|on", 0, "off: a FAT32 or exFAT stick offers only the disk images in its root, not its files." },
    { "mac.exchange", PDG_G_STORAGE, 0, A, "*", "Mac Exchange", "The card's folder shown as a locked disk (Mac Exchange by default); off for none." },
    { "mac.sescsi", PDG_G_STORAGE, 0, PDG_SE, "auto|real|empty", 0,
      "The SE's SCSI bus: auto looks before the Mac starts; real always the SE's chip; empty answers as a bus with nothing on it." },
    { "mac.sescsiwait", PDG_G_STORAGE, 0, PDG_SE, "#", "250", "The SE's SCSI selection timeout in milliseconds, 1 to 1000 (default 250)." },
    { "mac.cardrom", PDG_G_STORAGE, 0, A, "off", 0, "off: the declaration ROMs in the card's root are not offered, only the container's." },

    /* ---- Network ---- */
    { "wifi.ssid", PDG_G_NETWORK, PDG_F_WIFITAB, A, "*", 0, "The WiFi network to join." },
    { "wifi.psk", PDG_G_NETWORK, PDG_F_WIFITAB | PDG_F_SECRET, A, "*", 0, "The WiFi password." },
    { "wifi.country", PDG_G_NETWORK, PDG_F_WIFITAB, A, "*", 0, "The WiFi's country, two capital letters." },
    { "wifi", PDG_G_NETWORK, 0, A, "off|on", 0, "off: the WiFi chip left off." },
    { "wifi.sdclock", PDG_G_NETWORK, 0, A, "#", "25000000", "The WiFi chip's SDIO clock in Hz, for a chip that misbehaves at the default." },
    { "eth", PDG_G_NETWORK, 0, A, "off|usb|on", 0, "The wired Ethernet: usb, a USB adapter as the wired card (Pi 3A+, Zero 2 W); off, none; on, started at once." },
    { "net.oui", PDG_G_NETWORK, 0, A, "off", 0, "off: the Pi's own MAC addresses (by default Commodore's prefix, as Alice68)." },
    { "bt", PDG_G_NETWORK, 0, A, "off|on", 0, "off: the Bluetooth chip left off." },
    { "bt.pair", PDG_G_NETWORK, 0, A, "*", "1", "1: pair the first keyboard or mouse in pairing mode; a NAME: only a device whose name holds it." },
    { "bt.dev", PDG_G_NETWORK, PDG_F_SECRET, A, "*", 0, "A paired device, as the log printed it after bt.pair (its key stays secret)." },
    { "bt.fwbaud", PDG_G_NETWORK, 0, A, "#", "921600", "The speed the Bluetooth chip's firmware is sent at (921600 by default; 115200 the old, slower way)." },
    { "remote", PDG_G_NETWORK, 0, A, "on|off", 0, "on: the remote control (the Mac's screen, mouse, keyboard and log over HTTP, docs/13)." },
    { "remote.token", PDG_G_NETWORK, PDG_F_SECRET, A, "*", 0, "The remote control's token, 8 characters or more." },
    { "remote.name", PDG_G_NETWORK, 0, A, "*", "aurora68-pi", "The remote control's .local name." },
    { "remote.ip", PDG_G_NETWORK, 0, A, "#.#.#.#/#,#.#.#.#|#.#.#.#/#", "A.B.C.D/24,ROUTER", "A fixed address for the remote control instead of DHCP: A.B.C.D/NN,ROUTER." },
    { "remote.link", PDG_G_NETWORK, 0, A, "wired|wifi", 0, "Which network the remote control uses." },
    { "remote.wifi", PDG_G_NETWORK, 0, A, "own", 0, "own: on the WiFi, an address of the remote's own instead of the Mac's with port 8068." },
    { "remote.any", PDG_G_NETWORK, 0, A, "1", 0, "1: answer requests from outside the local network too." },
    { "remote.mac", PDG_G_NETWORK, 0, A, "%:%:%:%:%:%", "XX:XX:XX:XX:XX:XX", "The remote control's own Ethernet address on the wired port." },
    { "recovery.ap", PDG_G_NETWORK, 0, A, "on|off", 0, "on: Setup and Recovery starts at once as a WiFi access point for a phone (the bench's way; the button is the usual one)." },
    { "recovery.apsec", PDG_G_NETWORK, 0, A, "wpa2|open", 0, "The access point's security: open, or WPA2 even when the firmware does not say it can." },
    { "recovery.apchannel", PDG_G_NETWORK, 0, A, "1|2|3|4|5|6|7|8|9|10|11|12|13", 0, "The access point's channel (6, allowed everywhere, by default)." },

    /* ---- Video card ---- */
    { "mac.video", PDG_G_VIDEO, 0, A, "off|on", 0, "off: no Aurora68 Video Decoder card in slot $C (QuickTime then decodes everything itself)." },
    { "mac.vdecode", PDG_G_VIDEO, 0, A, "off|on", 0, "off: the card's frame decoder starts switched off, for comparison with QuickTime's own." },
    { "mac.vbuffer", PDG_G_VIDEO, 0, A, "#", "8",
      "MB of the Pi's memory the card keeps behind its ring for a stream (music videos, Aurora68 Video): 8 by default, at most 32; 0 for the ring alone (512 KB)." },
    { "mac.vprefill", PDG_G_VIDEO, 0, A, "#|#,#", "3,2",
      "Seconds of a stream held before it starts, and after a comma before it goes on when the network ran it dry: 3,2 by default; 0 starts at once." },

#ifdef PSMAC_PC
    /* ---- PC card (a kernel built with -DPSMAC_PC=ON: psmac_pc.c, docs/19-pc-card.md) ---- */
    { "pc", PDG_G_PC, 0, A, "on|off", 0,
      "on: the Aurora68 PC Card in slot $D, a PC (Aurora86: Libreboot's firmware) in a window of the Aurora68 PC program; off by default." },
    { "pc.ram", PDG_G_PC, 0, A, "#", "16", "The PC's memory in MB: 16 by default, 4 to 256 (the Pi's own, beside about 104 MB the PC needs besides)." },
    { "pc.core", PDG_G_PC, 0, A, "1|2", 0, "The Pi core the PC runs on: by default 2 on the solo board, 1 (in slices beside the sound) on the SE and the Plus." },
    { "pc.rom", PDG_G_PC, 0, A, "*", "aurora86.rom", "The PC's firmware in the card's root: Libreboot's QEMU x86 image as Aurora86 builds it (aurora86.rom by default)." },
    { "pc.vgarom", PDG_G_PC, 0, A, "*", "aurora86-vga.bin", "The PC's VGA BIOS in the card's root (SeaVGABIOS for Bochs's VGA; aurora86-vga.bin by default)." },
    { "pc.disk0", PDG_G_PC, 0, A, "*", "freedos.img", "A hard disk image in the card's root for the PC's first IDE disk (16 heads, 63 sectors); what the PC writes stays in memory." },
#endif

    /* ---- Debug ---- */
    { "mac.buscheck", PDG_G_DEBUG, PDG_F_ONCE, C, "1|loop|force|force,loop", 0,
      "1: the bus between the Pi and the logic board measured instead of starting the Mac (one page on its screen, buscheck.txt); loop: again and again; force: over the testbed's bridge too." },
    { "mac.jtag", PDG_G_DEBUG, 0, C, "off|on", 0, "off: leave the CPLD's JTAG pins alone (by default its usercode is read once at start)." },
    { "mac.logsave", PDG_G_DEBUG, PDG_F_ONCE, A, "#", "10", "Seconds between saves of the kernel's log into aurora68.log on the card (0: never; the SE board's default 30, others' 0)." },
    { "mac.synclog", PDG_G_DEBUG, PDG_F_ONCE, A, "1", 0, "1: every log character sent to the serial line before going on (slower, but nothing is lost if the Pi stops)." },
    { "mac.vframelog", PDG_G_DEBUG, PDG_F_ONCE, A, "1|0", 0, "1: the video decoder card logs every picture it decodes." },
    { "mac.scsitrace", PDG_G_DEBUG, PDG_F_ONCE, PDG_SE | PDG_PLUS | PDG_IICI, "#", "200", "The first N accesses to the SCSI chip in the log." },
    { "usb.trace", PDG_G_DEBUG, PDG_F_ONCE, A, "1|2", 0, "1: every failed USB transfer in full (the channel's bits per split, its registers); 2: also the keyboard's toggle set again every 10 s, a test." },
    { "mac.solotrace", PDG_G_DEBUG, PDG_F_ONCE, PDG_SOLO, "#", "200", "The solo board's first N SCSI and ADB commands in the log." },
    { "mac.setrace", PDG_G_DEBUG, PDG_F_ONCE, C, "#|#,*", "200,VIA", "The first N accesses to the compact Mac's chips in the log (N,VIA or N,SCC: of one chip)." },
    { "mac.plustrace", PDG_G_DEBUG, PDG_F_ONCE, PF, "#", "200", "The first N ADB commands of the Plus's keyboard and mouse layer in the log." },
    { "mac.pluskeylog", PDG_G_DEBUG, PDG_F_ONCE, PF, "#", "100", "The first N bytes from the Plus's own keyboard in the log." },
    { "mac.plusdcd", PDG_G_DEBUG, 0, PF, "keep", 0, "keep: the SCC's DCD interrupts as the Mac sets them (by default off, since the mouse is on DCD)." },
    { "mac.pram8a", PDG_G_DEBUG, 0, C, "keep", 0, "keep: parameter RAM byte $8A as the Mac writes it (by default 32-bit addressing is kept on above 8 MB)." },
    { "mac.bustrace", PDG_G_DEBUG, PDG_F_ONCE, BUS, "#", "400", "The first N bus cycles to the logic board in the log." },
    { "mac.buswait", PDG_G_DEBUG, PDG_F_ONCE, BUS, "1", 0, "1: how long the 68k waited for the bus, at each heartbeat." },
    { "mac.peek", PDG_G_DEBUG, PDG_F_ONCE, A, "%,#|%", "910,32", "Mac memory at HEXADDRESS (and LENGTH bytes, 64 by default) dumped at each heartbeat." },
    { "mac.pcwatch", PDG_G_DEBUG, PDG_F_ONCE, A, "%", "4080a000", "The 68k's recent PCs in the log when its PC first reaches the page of HEXADDRESS." },
    { "mac.pctrail", PDG_G_DEBUG, PDG_F_ONCE, A, "#", "60", "The 68k's recent PCs in the log N seconds after the start." },
    { "mac.watchtraps", PDG_G_DEBUG, PDG_F_ONCE, A, "1|0", 0, "1: odd writes into the OS trap tables reported (costs a trap on every write to low memory)." },
    { "mac.dbg", PDG_G_DEBUG, 0, A, "%,%", "40800000,40800100", "The JIT's translation of blocks between two hex addresses in the log." },
    { "mac.dbfslow", PDG_G_DEBUG, 0, A, "0|1|2|3|4|5|6|7|8", 0, "Device reads per turn of a self-timing DBF loop (1 by default; 0 makes such loops free)." },
    { "mac.xpram", PDG_G_DEBUG, 0, PDG_SOLO, "*", "e0:098100a8", "Extended parameter RAM preset, ADDR:HEXBYTES[,ADDR:HEXBYTES] (the solo board)." },
};
const unsigned pdg_nopts = sizeof pdg_opts / sizeof pdg_opts[0];

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ieq(const char *a, const char *b) { while (*a && *b) if (lower(*a++) != lower(*b++)) return 0; return !*a && !*b; }
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int isdig(int c) { return c >= '0' && c <= '9'; }
static int ishex(int c) { return isdig(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
/* a small formatter: %s only, into a bounded buffer */
static void say(char *out, uint32_t cap, const char *a, const char *b, const char *c, const char *d)
{
    const char *p[4] = { a, b, c, d }; uint32_t o = 0;
    for (int i = 0; i < 4; i++) for (const char *s = p[i]; s && *s && o + 1 < cap; s++) out[o++] = *s;
    if (cap) out[o] = 0;
}

const struct pdg_opt *pdg_find(const char *key)
{
    for (unsigned i = 0; i < pdg_nopts; i++) if (ieq(pdg_opts[i].key, key)) return &pdg_opts[i];
    return 0;
}
int pdg_board(const char *v)
{
    if (!v || !v[0]) return PDG_IICI;
    static const char *const w[] = { "se", "plus", "512ke", "512k", "128k", "solo" };
    for (int i = 0; i < 6; i++) if (ieq(v, w[i])) return 1 << i;
    return PDG_IICI;
}
const char *pdg_board_word(int bit)
{
    static const char *const w[] = { "se", "plus", "512ke", "512k", "128k", "solo", "iici" };
    for (int i = 0; i < 7; i++) if (bit == 1 << i) return w[i];
    return "iici";
}
int pdg_uses(const struct pdg_opt *o, int board) { return o && (o->boards & board) != 0; }
int pdg_choices(const struct pdg_opt *o)
{
    for (const char *s = o->values; *s; s++) if (*s == '#' || *s == '%' || *s == '*') return 0;
    return 1;
}
/* one alternative [a, e) against the whole of v */
static int alt_ok(const char *a, const char *e, const char *v)
{
    while (a < e) {
        if (*a == '#') { if (!isdig(*v)) return 0; while (isdig(*v)) v++; a++; continue; }
        if (*a == '%') {
            if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X') && ishex(v[2])) v += 2;
            if (!ishex(*v)) return 0;
            while (ishex(*v)) v++;
            a++; continue;
        }
        if (*a == '*') {
            if (!*v) return 0;
            for (; *v; v++) if ((unsigned char)*v < 32 || *v == '"' || (unsigned char)*v > 126) return 0;
            a++; continue;
        }
        if (lower(*a) != lower(*v)) return 0;
        a++; v++;
    }
    return !*v;
}
int pdg_value_ok(const struct pdg_opt *o, const char *v, char *why, uint32_t whymax)
{
    if (!o) return 1;
    for (const char *a = o->values; *a; ) {
        const char *e = a; while (*e && *e != '|') e++;
        if (alt_ok(a, e, v)) return 1;
        a = *e ? e + 1 : e;
    }
    if (why) {
        const char *shown = pdg_choices(o) ? o->values : o->example ? o->example : o->values;
        say(why, whymax, o->key, ": ", v, pdg_choices(o) ? " is not one of " : " is not a value it takes, such as ");
        uint32_t n = slen(why);
        say(why + n, whymax > n ? whymax - n : 0, shown, 0, 0, 0);
    }
    return 0;
}

/* ---- the one-shot debug start ---- */
struct once_ctx { int bad; char *why; uint32_t whymax; int n; };
static void once_word(void *vc, const char *w, uint32_t wl)
{
    struct once_ctx *x = vc;
    char key[48], val[160]; uint32_t k = 0, i = 0;
    x->n++;
    if (x->bad) return;
    while (i < wl && w[i] != '=' && k + 1 < sizeof key) key[k++] = w[i++];
    key[k] = 0;
    if (i >= wl || w[i] != '=') { x->bad = 1; say(x->why, x->whymax, key, ": a one-shot debug start takes key=value words only", 0, 0); return; }
    i++;
    uint32_t v = 0;
    if (i < wl && w[i] == '"') { i++; while (i < wl && w[i] != '"' && v + 1 < sizeof val) val[v++] = w[i++]; }
    else while (i < wl && v + 1 < sizeof val) val[v++] = w[i++];
    val[v] = 0;
    const struct pdg_opt *o = pdg_find(key);
    if (!o || !(o->flags & PDG_F_ONCE)) { x->bad = 1; say(x->why, x->whymax, key, " is not a debug option a one-shot start takes", 0, 0); return; }
    if (!pdg_value_ok(o, val, x->why, x->whymax)) { x->bad = 1; return; }
}
int pdg_once_check(const char *text, uint32_t n, char *why, uint32_t whymax)
{
    if (pcl_check_line(text, n, why, whymax)) return -1;
    struct once_ctx x = { 0, why, whymax, 0 };
    pcl_words(text, n, once_word, &x);
    if (x.bad) return -1;
    if (!x.n) { say(why, whymax, "no options: an empty one-shot debug start", 0, 0, 0); return -1; }
    return 0;
}
struct merge_ctx { char *out; uint32_t cap; int len, merged, skipped, full; char *why; uint32_t whymax; char tmp[4096]; };
static void merge_word(void *vc, const char *w, uint32_t wl)
{
    struct merge_ctx *x = vc;
    if (x->full) return;
    char one[256], why[160]; uint32_t k = 0;
    for (; k < wl && k + 1 < sizeof one; k++) one[k] = w[k];
    one[k] = 0;
    if (pdg_once_check(one, k, why, sizeof why)) { if (!x->skipped++) say(x->why, x->whymax, why, 0, 0, 0); return; }
    char key[48], val[160]; uint32_t i = 0, kk = 0, v = 0;
    while (i < k && one[i] != '=' && kk + 1 < sizeof key) key[kk++] = one[i++];
    key[kk] = 0; i++;
    if (i < k && one[i] == '"') { i++; while (i < k && one[i] != '"' && v + 1 < sizeof val) val[v++] = one[i++]; }
    else while (i < k && v + 1 < sizeof val) val[v++] = one[i++];
    val[v] = 0;
    if (x->cap > sizeof x->tmp) { x->full = 1; return; }
    int r = pcl_set(x->out, (uint32_t)x->len, x->tmp, x->cap, key, val);
    if (r < 0) { if (r == -1) x->full = 1; else if (!x->skipped++) say(x->why, x->whymax, key, ": the value cannot be written", 0, 0); return; }
    for (int j = 0; j < r; j++) x->out[j] = x->tmp[j];
    x->len = r; x->merged++;
}
int pdg_once_merge(const char *args, uint32_t an, const char *once, uint32_t on, char *out, uint32_t cap,
                   int *merged, int *skipped, char *skip_why, uint32_t whymax)
{
    static struct merge_ctx x;
    if (an + 1 > cap) return -1;
    for (uint32_t i = 0; i < an; i++) out[i] = args[i];
    out[an] = 0;
    x.out = out; x.cap = cap - 1; x.len = (int)an; x.merged = x.skipped = x.full = 0; x.why = skip_why; x.whymax = whymax;
    if (skip_why && whymax) skip_why[0] = 0;
    /* the file may end in a newline (an editor's); one line only, as cmdline.txt */
    while (on && (once[on - 1] == '\n' || once[on - 1] == '\r' || once[on - 1] == ' ')) on--;
    pcl_words(once, on, merge_word, &x);
    *merged = x.merged; *skipped = x.skipped;
    if (x.full) return -1;
    out[x.len] = 0;
    return x.len;
}

/* ---- the bundle's secrets ---- */
static int secret_key(const char *s, uint32_t n)
{
    uint32_t b = n;
    while (b && s[b - 1] != '.') b--;                  /* the last part of a dotted name */
    static const char *const names[] = { "psk", "pass", "passwd", "password", "passphrase", "passkey", "key", "secret", "token", "pin", "recoverypin" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        uint32_t m = slen(names[i]), j = 0;
        if (n - b < m) continue;
        while (j < m && lower(s[n - m + j]) == names[i][j]) j++;
        if (j == m && (n - m == b || s[n - m - 1] == '_' || s[n - m - 1] == '-')) return 1;
    }
    return 0;
}
static int name_is(const char *s, uint32_t n, const char *k) { uint32_t j = 0; while (j < n && k[j] && lower(s[j]) == k[j]) j++; return j == n && !k[j]; }
static int namec(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isdig(c) || c == '.' || c == '_' || c == '-'; }
uint32_t pdg_scrub(const char *in, uint32_t n, char *out, uint32_t cap, const char *const *secrets, int nsecrets)
{
    static const char hidden[] = "(hidden)";
    uint32_t o = 0, ns = 0;
    if (!cap) return 0;
    for (uint32_t i = 0; i < n && o + 1 < cap; i++) {
        /* a literal secret anywhere */
        int hit = 0;
        for (int s = 0; s < nsecrets && !hit; s++) {
            const char *z = secrets[s]; uint32_t m = z ? slen(z) : 0;
            if (m < 6 || i + m > n) continue;
            uint32_t j = 0; while (j < m && in[i + j] == z[j]) j++;
            if (j == m) { for (const char *h = hidden; *h && o + 1 < cap; h++) out[o++] = *h; i += m - 1; hit = 1; }
        }
        if (hit) { ns = o; continue; }
        char c = in[i];
        out[o++] = c;
        if (c != '=') { if (!namec((unsigned char)c)) ns = o; continue; }
        const char *nm = out + ns; uint32_t nl = o - 1 - ns;
        int hide = nl && secret_key(nm, nl), skip_to = 0;
        if (!hide && name_is(nm, nl, "mac.wifisim")) { hide = 1; skip_to = 1; }    /* SSID:PASSWORD */
        if (!hide && name_is(nm, nl, "bt.dev")) { hide = 1; skip_to = 1; }         /* ADDRESS:KEY:KIND */
        if (!hide) { ns = o; continue; }
        uint32_t v = i + 1, keep = 0, q = v < n && in[v] == '"';
        if (skip_to) { while (v + keep < n && in[v + keep] != ':' && in[v + keep] != ' ' && in[v + keep] != '\n') keep++; if (v + keep < n && in[v + keep] == ':') keep++; }
        for (uint32_t k = 0; k < keep && o + 1 < cap; k++) out[o++] = in[v + k];
        uint32_t e = v + keep;
        if (q && !keep) { e++; while (e < n && in[e] != '"' && in[e] != '\n') e++; if (e < n && in[e] == '"') e++; }
        else while (e < n && in[e] != ' ' && in[e] != '\t' && in[e] != '\n' && in[e] != '\r' && in[e] != ')' && in[e] != ',' && in[e] != '&' && !(skip_to && in[e] == ':')) e++;
        if (e > v + keep) for (const char *h = hidden; *h && o + 1 < cap; h++) out[o++] = *h;   /* an empty value stays empty */
        i = e - 1; ns = o;
    }
    out[o] = 0;
    return o;
}

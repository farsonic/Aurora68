/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 kernel Bluetooth: a small Classic (BR/EDR) host for keyboards and
 * mice, host-independent. bt_pi.c gives it the Pi's UART; tools/bt-test runs
 * it on the PC against scripted controller events.
 *
 *   H4 transport -> HCI (commands with the controller's flow control, events,
 *   ACL with fragments) -> Broadcom firmware download (.hcd) -> L2CAP
 *   signalling -> HID host on PSM 0x11/0x13 in boot protocol -> keyboard
 *   reports (8 bytes, as USB) and mouse movements.
 *   Pairing: Secure Simple Pairing (we display a passkey for keyboards, Just
 *   Works for mice) and legacy PIN 0000; link keys kept by the caller.
 *
 *   LE (btle.c, included by bthost.c): HID over GATT - LE connections, SMP
 *   legacy pairing (Just Works for mice, a passkey we show for keyboards; the
 *   controller's LE Encrypt does the AES), bonding with the LTK, GATT
 *   discovery of the HID service, boot protocol (report mode from the Report
 *   Map when a device has no boot reports), reconnects by the controller's
 *   filter accept list (and resolving list, for devices with an IRK).
 *
 * Written from the Bluetooth Core Specification (HCI, L2CAP, SSP, GAP, ATT,
 * GATT, SMP), the HID profile and HID over GATT / HID Service specifications,
 * and Linux's btbcm/hci_bcm for the Broadcom vendor commands. No structs over
 * wire data: every field byte by byte.
 */
#ifndef ALICE68_BTHOST_H
#define ALICE68_BTHOST_H
#include <stdint.h>

#define BT_MAXKEY    6                  /* paired devices */
#define BT_MAXFOUND  12                 /* scan results */
#define BT_MAXCONN   4                  /* links at once */
#define BT_NAME      40

enum { BT_OFF, BT_FW, BT_INIT, BT_READY, BT_FAILED };
#define BTK_CONNECTED 1u
#define BTK_KEYBOARD  2u
#define BTK_MOUSE     4u
#define BTK_LE        8u                /* a Bluetooth LE device: key = its LTK, with ediv and rand */

struct bt_key {
    uint8_t addr[6]; uint8_t key[16]; uint8_t type; uint32_t flags; char name[BT_NAME];
    /* LE: addr is the identity address; addr_type 0 public, 1 random */
    uint8_t addr_type; uint16_t ediv; uint8_t rand[8]; uint8_t irk[16]; uint8_t has_irk;
    /* what the host saw of it while it was away, for the heartbeat (bt_pi.c; 24 November 2026): the last advertisement
       heard (when, its event type and address type, the address it used), and the last thing that went wrong */
    uint32_t heard_ms, heard_n; uint8_t heard_evt, heard_atype, heard_addr[6];
    uint32_t err_ms; uint8_t err_code; uint8_t err_what;   /* BT_KE_*, with the HCI status or reason */
    uint32_t tries;                     /* connections the host asked for to it */
    uint8_t floor_drops;                /* it went within BT_LE_DROP_MS of a refusal under the floor (docs/08 item 147) */
};
enum { BT_KE_NONE, BT_KE_CONN, BT_KE_REFUSED, BT_KE_NOANSWER, BT_KE_ENC, BT_KE_DISC, BT_KE_KEY };

/* The radio's time (docs/08 item 140). On the Pi 3B the BCM43438 has one antenna for WiFi and Bluetooth, and the
   WiFi firmware's coexistence (btc_mode=1 in brcmfmac43430-sdio.txt, time division) gives Bluetooth the antenna
   whenever it scans. What the host leaves running while a paired device is away is therefore bounded:
     fast   page scan 11.25 ms every 160 ms, interlaced (14 %: Linux's "fast connectable"), LE scan 30 ms every
            160 ms (19 %; Linux's background scan, 30 ms every 60 ms, is 50 %, and was the host's until docs/08 item
            140); for BT_FAST_MS after the start, a Connect or a pairing asked for, and a link going down (that last
            at most once per BT_FAST_MS + BT_REWAKE_MS, so at most a third of the time)
     slow   page scan 11.25 ms every 1.28 s, standard (0.9 %: the Core specification's and Linux's default), LE scan
            30 ms every 1.28 s (2.3 %)
     idle   after BT_IDLE_MS more with no paired device heard: no LE scan but a sweep of BT_SWEEP_MS (at the fast
            parameters) in every BT_SWEEP_EVERY (0.6 % on average); page scan as slow
     off    (bt.scan=off) no page scan and no LE scan at all, except for BT_FAST_MS after a Connect or a pairing
   and over all of them, the WiFi's DHCP (bt_hush, from alice_net.c when the Mac sends a DHCP request over the WiFi):
   no page scan and no LE scan for BT_HUSH_MS, at most once per BT_HUSH_MS + BT_HUSH_GAP (Linux's brcmfmac boosts the
   WiFi during DHCP for the same reason, btcoex.c)
   Page scan runs only while a paired Classic device is away or a pairing is going on, as Linux's (no disconnected
   entry in its accept list, no page scan). A scan for new devices (bt_scan) is the user's and runs at full time. */
enum { BT_SCAN_AUTO, BT_SCAN_FAST, BT_SCAN_SLOW, BT_SCAN_OFF };      /* bt.scan=auto|fast|slow|off (bt_pi.c) */
enum { BT_TIER_FAST, BT_TIER_SLOW, BT_TIER_IDLE, BT_TIER_OFF };
#define BT_FAST_MS   60000u
#define BT_REWAKE_MS 120000u
#define BT_IDLE_MS   300000u
#define BT_SWEEP_MS  10000u
#define BT_SWEEP_EVERY 300000u
#define BT_HUSH_MS   10000u
#define BT_HUSH_MAX  15000u
#define BT_HUSH_GAP  5000u
#define BT_CMD_TIMEOUT 2000u            /* Linux's HCI_CMD_TIMEOUT */
/* LE connection parameters (docs/21, 25 November 2026): what the host asks for when it connects, Linux's defaults
   (hci_core.c: le_conn_min_interval 0x0018, le_conn_max_interval 0x0028, latency 0); until then 7.5 to 15 ms, three to
   six times the radio time of every link on the Pi 3B's shared antenna. The supervision timeout stays 5 s (Linux's is
   420 ms): a link that the WiFi's share of the antenna starves for a moment is kept. A device's own request for shorter
   intervals is taken (it knows what it needs), but never below BT_LE_FLOOR when its range reaches that far. */
#define BT_LE_CI_MIN  0x0018u
#define BT_LE_CI_MAX  0x0028u
#define BT_LE_TIMEOUT 0x01f4u
#define BT_LE_FLOOR   0x000cu           /* 15 ms */
/* The floor that holds while the WiFi is in use (docs/08 item 147, 25 November 2026). On the owner's SE two LE HID devices
   each asked, after a restart, for 7.5 ms and nothing longer, and were given it (BT_LE_FLOOR above only raised a range
   that reached 15 ms): the central transmits at every connection event whatever the peripheral latency, and two links
   at 7.5 ms took so much of the Pi 3B's shared antenna that the access points dropped the WiFi station; with bt=off the
   WiFi stayed up. Now, while the WiFi is scanning, joining or connected (bt_pi.c bt_pi_wifi_use, from alice_net.c), a
   request whose longest interval is under the floor is refused (L2CAP result 1, as Linux's l2cap_conn_param_update_req
   refuses one outside its bounds): the link keeps what it was connected with (30 to 50 ms). A link that already runs
   under the floor is asked up to it (LE Connection Update, twice at most). With two or more LE links up the floor is half
   as long again (15 ms: 22.5 ms). A device that disconnects within BT_LE_DROP_MS of a refusal BT_LE_FLOOR_DROPS times is
   given what it asks for from then on (a keyboard that works beats one that keeps leaving), and with the WiFi off
   every valid request is taken as before. bt.lefloor=MS sets the floor (bt_pi.c); bt.lefloor=0 is the old behaviour. */
#define BT_LE_FLOOR_DROPS 3u
#define BT_LE_DROP_MS     10000u
#define BT_LE_RAISE_TRIES 2u
struct bt_radio {                       /* what the host has the controller do now, for the heartbeat (bt_radio_now) */
    int mode, tier;                     /* BT_SCAN_*, BT_TIER_* */
    uint32_t tier_ms;                   /* how long the tier has left before the next (0: for good) */
    int page_on, page_interlaced; uint16_t page_int, page_win;     /* 0.625 ms units */
    int le_on; uint16_t le_int, le_win; /* the host's own LE scan or the controller's accept list */
    int inquiry;                        /* a scan for new devices (full time while it lasts) */
    uint32_t hush_ms;                   /* the WiFi's DHCP: no scans for this long */
    int sweep; uint32_t sweep_in;       /* idle: the LE sweep runs now; else the next in this many ms */
    unsigned permille;                  /* page + LE scan's share of the radio, in thousandths (not the inquiry) */
};

struct bt_found { uint8_t addr[6]; uint32_t cod; int8_t rssi; uint8_t psr; uint16_t clkoff; char name[BT_NAME]; int named; uint8_t le, addr_type; };   /* le: a Bluetooth LE device; addr_type as advertised */

struct bt_io {
    void *ctx;
    void (*send)(void *ctx, const uint8_t *p, unsigned n);        /* bytes to the controller (H4) */
    void (*set_baud)(void *ctx, uint32_t baud);                   /* the UART's speed, once what is queued is out */
    uint32_t (*ms)(void *ctx);                                    /* a millisecond clock */
    void (*log)(void *ctx, const char *line);                     /* one line, no newline */
    void (*keyboard)(void *ctx, const uint8_t rep[8]);            /* a boot keyboard report */
    void (*mouse)(void *ctx, int dx, int dy, int wheel, unsigned buttons);
    void (*keys)(void *ctx);                                      /* the paired list changed: save it */
    void (*kick)(void *ctx, int attempt);                         /* the reset after the firmware went unanswered: unstick the line (may be 0) */
    uint32_t fw_baud;                                             /* the .hcd sent at this speed (the vendor's Update UART Baud Rate,
                                                                     as Linux's hci_bcm and hciattach do); 0: at 115200 */
};

struct bt_l2ch { uint16_t psm, lcid, rcid; uint8_t state, conf; };
/* an LE link: pairing (SMP) and the HID service as GATT found it */
#define BT_LE_CHARS 16
#define BT_LE_REPORTS 8
struct bt_kbrep { uint8_t id, has_mod, nkeys, bm_n, bm_min; uint16_t mod_pos, keys_pos, bm_pos; };   /* one keyboard input report */
struct bt_kbmap { uint8_t ok, n; struct bt_kbrep r[3]; };     /* report mode: key arrays (6KRO) and key bitmaps (NKRO), by report id */
struct bt_le {
    uint8_t addr_type, rat, ra[6];       /* identity type; the address (and type) the link was made with, for c1 */
    uint8_t rekey;                       /* encrypting with the stored LTK */
    /* SMP */
    uint8_t smp, mitm, passkey_method, rdist, got;
    uint8_t preq[7], pres[7], tk[16], mrand[16], srand[16], sconf[16], conf[16], ltk[16], rand[8], irk[16], idaddr[7];
    uint16_t ediv; uint8_t have_sconf, have_srand, keysize;
    uint32_t smp_until;
    /* GATT */
    uint8_t g, gi, att_busy;
    uint16_t svc_s, svc_e, cursor, mtu;
    uint16_t chr_vh[BT_LE_CHARS], chr_uuid[BT_LE_CHARS], chr_cccd[BT_LE_CHARS], chr_ref[BT_LE_CHARS]; uint8_t nchr;
    uint8_t rep_id[BT_LE_CHARS], rep_type[BT_LE_CHARS];
    uint16_t pm, kb_in, ms_in, map;      /* value handles (0: none) */
    uint8_t boot;                        /* boot protocol in use */
    uint8_t mouse_ok; uint8_t hmouse[40];   /* struct hid_mouse (btle.c), opaque here */
    struct bt_kbmap kbm;
    uint8_t lastbtn;
    uint16_t ci, cl, cto;                /* the link's connection interval (1.25 ms), latency, supervision timeout (10 ms): what
                                            the controller reports at the link's start and at each update (docs/21) */
    uint16_t upd_asked, upd_refused;     /* the device's Connection Parameter Update Requests: taken, refused */
    uint16_t upd_floored;                /* refused because the WiFi is in use and they were under the floor (docs/08 item 147) */
    uint32_t up_ms, floored_ms;          /* when the link came up; the last such refusal (0: none) */
    uint8_t raise_tries; uint32_t raise_at;   /* LE Connection Updates of ours up to the floor, and when the next may go */
    uint8_t nlog;                        /* notifications logged on this link (the first few) */
    /* what the device presented, for the heartbeat (bt_pi.c): a bench has no scrollback */
    uint16_t svcs[3][2]; uint8_t nsvc, svci;   /* HID service instances (start, end), the one in use */
    uint8_t att_err, att_err_g;          /* the last ATT error and the discovery stage it came in */
    uint16_t map_len; uint8_t map_head[48];
    uint32_t nnotify;
    uint32_t up_frames, up_att, up_other; uint16_t up_last_cid; uint8_t up_last_op;   /* what arrived after the link was up */
};
struct bt_conn {
    int used;
    uint16_t handle;
    uint8_t addr[6];
    int out;                            /* we connected (and open the channels) */
    int authed, encrypted, pairing, hid_up;
    uint8_t remote_io;                  /* from IO Capability Response, 0xff unknown */
    uint8_t sig_id;
    struct bt_l2ch ch[3];               /* HID control, HID interrupt, SDP */
    uint8_t acl[700];                   /* an L2CAP frame being put together */
    unsigned acl_n, acl_want;
    uint8_t lastkb[8];
    uint16_t sent;                      /* ACL packets given to the controller and not yet completed (Number Of Completed
                                           Packets): handed back to the pool when the link goes, as Linux's hci_conn_del does */
    int le;                             /* an LE link (fixed channels: ATT 4, SMP 6, LE signalling 5) */
    struct bt_le l;
};

#define BT_CMDQ 24
#define BT_ACLQ 8
struct bt_host {
    const struct bt_io *io;
    int state;
    char why[64];                       /* BT_FAILED: why */
    uint8_t bdaddr[6];
    uint8_t want_addr[6]; int want_addr_set;  /* bt.local= (pairings.txt): the address the chip is given after its own is read, HCI order */
    /* the controller */
    const uint8_t *fw; unsigned fwlen, fwpos, fwrec;
    int step;                           /* where the start-up is */
    uint32_t wait_until;
    int credits;                        /* HCI commands the controller takes now */
    uint16_t pending_op;                /* the command whose answer we wait for in the start-up, 0 none */
    uint8_t cmdq[BT_CMDQ][260]; uint16_t cmdq_len[BT_CMDQ]; unsigned cq_head, cq_tail;
    uint16_t acl_mtu, acl_free, acl_pkts;   /* acl_pkts: the controller's ACL buffers (the pool never grows past it) */
    uint8_t aclq[BT_ACLQ][260]; uint16_t aclq_len[BT_ACLQ]; uint8_t aclq_le[BT_ACLQ]; unsigned aq_head, aq_tail;
    uint16_t le_mtu, le_free, le_pkts;  /* the controller's LE buffers (0: LE shares acl_free) */
    /* a command whose Command Complete or Command Status never came (bytes lost on a UART without flow control, the
       Pi 3B's): after BT_CMD_TIMEOUT the host takes the controller's credit back and goes on, as Linux's hci_cmd_timeout
       does (docs/21); before 25 November 2026 one lost answer stopped every later command for good */
    uint16_t cmd_out_op; uint32_t cmd_out_ms; int cmd_waiting; uint32_t cmd_timeouts;
    uint32_t rx_resyncs, rx_breaks;     /* H4 headers that made no sense (dropped, the stream searched again); packets cut by a UART error */
    /* the H4 receiver */
    uint8_t rx[1100]; unsigned rx_n, rx_need; int rx_type;
    /* devices */
    struct bt_key keys[BT_MAXKEY]; int nkeys; uint32_t keys_gen;
    struct bt_found found[BT_MAXFOUND]; int nfound;
    int scanning; uint32_t scan_end;
    int names_pending;
    struct bt_conn conn[BT_MAXCONN];
    uint8_t pair_addr[6]; int pairing;  /* a pairing the user asked for */
    uint32_t passkey; int passkey_shown;
    uint16_t next_cid;
    char status[64];                    /* in a few words, for the splash and the tool */
    uint32_t reconnect_at; int reconnect_next;
    uint8_t call_addr[6]; int calling;  /* a Create Connection of ours is out */
    int tries;                          /* resets sent after the firmware's launch */
    int le_scanning;                    /* an LE scan beside the inquiry: LE devices are named, not served */
    unsigned heard_other, heard_logged; /* devices a scan heard that are no keyboard or mouse */
    unsigned scan_ir, scan_eir, scan_le; /* inquiry results, extended ones, LE reports in this scan (all, before any filter) */
    /* LE */
    int le_init;                        /* an LE Create Connection is out: 1 to one device, 2 the accept list */
    uint32_t le_init_until;             /* 1: give up then; 0: not again before then */
    int le_cancelling;                  /* an LE Create Connection Cancel is out */
    int le_lists_dirty;                 /* the accept/resolving lists must be written again */
    int aes_busy; uint16_t aes_handle, aes_op;   /* an LE Encrypt / LE Rand of SMP's is out, for that link (the answer must be that command's) */
    uint8_t rmap[1024]; unsigned rmap_n; /* a Report Map being read (one link at a time) */
    /* LE devices with a resolvable private address on a controller without the resolving list (Bluetooth 4.1, the
       Pi 3B's BCM43438): a passive scan of the host's own, each address resolved here with the device's IRK */
    int no_resolv;                      /* the controller refused the resolving list's commands */
    int bg_scan, bg_said;               /* that scan is on; the log has said so */
    int rpa_key; uint8_t rpa_addr[6];   /* the key whose device answered at rpa_addr: le_init 3 is connecting to it */
    /* the firmware download at fw_baud: 0 not used, 1 the line is at fw_baud now, 2 used and back at 115200 */
    int fast; uint32_t fw_t0;
    int cur_conn;                       /* the link (index into conn[]) whose report the keyboard and mouse callbacks carry now:
                                           the kernel keeps each device's keys and buttons apart (alice_psmac.c, 24 November 2026) */
    int bg_restarts;                    /* the host's own scan started again so that its duplicate filter forgets (le_poll) */
    uint32_t bg_since;                  /* when it was started */
    /* the radio's time (docs/08 item 140, above): bt.scan=, kept by bt_init */
    int scan_mode;
    uint32_t wake_ms;                   /* the last start of the fast tier (or of the slow one, with bt.scan=slow) */
    const char *wake_why;               /* why, for the log */
    uint32_t user_ms; int user_set;     /* the last Connect or pairing asked for */
    uint32_t heard_any_ms;              /* a paired device last heard advertising (keeps the slow tier from going idle) */
    int ps_on, ps_set, ps_fast;         /* the page scan as last written: on, its activity written, fast */
    int bg_tier, init_tier;             /* the tier the host's LE scan, the accept list's connection, were started with */
    int tier_said;                      /* the last tier logged */
    uint32_t hush_until; int hush_set, hush_said; uint32_t hush_n;   /* bt_hush */
    /* the LE interval floor while the WiFi is in use (above): kept by bt_init, like scan_mode */
    uint16_t le_floor; int le_floor_given;   /* 1.25 ms units, 0 none (bt.lefloor=0); given: bt.lefloor= was read */
    volatile int wifi_inuse;            /* the WiFi scans, joins or is connected (bt_pi_wifi_use) */
    uint32_t le_floored, le_floor_drops; /* requests refused under the floor, devices that went after one: for the heartbeat */
};

/* fw/fwlen: the .hcd file (0: none - the controller's ROM firmware is used) */
void bt_init(struct bt_host *h, const struct bt_io *io, const uint8_t *fw, unsigned fwlen);
void bt_rx(struct bt_host *h, const uint8_t *p, unsigned n);       /* bytes from the controller */
void bt_rx_break(struct bt_host *h);                               /* the UART lost bytes (overrun, framing): the packet in hand is cut */
void bt_poll(struct bt_host *h);                                   /* timers; call often */
/* a paired device known from before (cmdline.txt), before or after bt_init */
int  bt_add_key(struct bt_host *h, const uint8_t addr[6], const uint8_t key[16], uint8_t type, uint32_t kind);
/* an LE one: identity address and type, LTK, EDIV, Rand, IRK (0: none) */
int  bt_add_le_key(struct bt_host *h, const uint8_t addr[6], uint8_t addr_type, const uint8_t ltk[16], uint16_t ediv,
                   const uint8_t rand[8], const uint8_t *irk, uint32_t kind);
int  bt_scan(struct bt_host *h);                                   /* ~10 s inquiry: found[] fills */
int  bt_pair(struct bt_host *h, const uint8_t addr[6]);
int  bt_connect(struct bt_host *h, const uint8_t addr[6]);         /* a paired device, now */
int  bt_forget(struct bt_host *h, const uint8_t addr[6]);
/* a resolvable private address made from this IRK (as SMP gives it, least significant octet first)? 1 yes (btle.c) */
int  bt_rpa_match(const uint8_t irk[16], const uint8_t addr[6]);
void bt_aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);   /* FIPS-197 byte order */
/* the heartbeat's words for what the host is doing about a paired device that is not connected (btle.c) */
const char *bt_key_activity(struct bt_host *h, int key, uint32_t *ms);
/* the radio's time now (bthost.c); the tier's name */
void bt_radio_now(struct bt_host *h, struct bt_radio *r);
const char *bt_tier_word(int tier);
/* the WiFi is getting its address (DHCP): no background scans for ms (bounded, bthost.h above) */
void bt_hush(struct bt_host *h, uint32_t ms, const char *why);
/* a Connection Parameter Update Request valid? The interval's lower end the host asks of the controller (btle.c) */
int bt_le_params_ok(uint16_t mn, uint16_t mx, uint16_t lat, uint16_t to);
uint16_t bt_le_min_floor(uint16_t mn, uint16_t mx);
/* the floor in force (1.25 ms units): bt.lefloor's, half as long again with two or more LE links up; 0 none */
uint16_t bt_le_floor_now(uint16_t floor, unsigned le_links);
/* a valid request whose longest interval is mx: refuse it (1) or take it (0)? (docs/08 item 147) */
int bt_le_floor_refuse(uint16_t floor_now, int wifi_inuse, uint16_t mx, unsigned drops);
/* the words for an HCI status or disconnection reason (bthost.c) */
const char *bt_status_words(uint8_t st);
/* "AA:BB:CC:DD:EE:FF" (addr as the controller gives it: least significant byte first) */
void bt_addr_str(const uint8_t addr[6], char out[18]);
#endif

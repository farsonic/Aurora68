/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_files.c - the remote control's files and programs: list, read, write, make, delete and rename
 * files and folders on the Mac's disks, open programs and documents, list and quit programs (docs/13,
 * "Files and applications").
 *
 * Backstory. With the screen, the mouse and the log on the MacBook (psmac_remote.c, 20 November 2026)
 * the owner asked for "a command line on the client for jobs like this. Screen shots. Push files etc"
 * (21 November). The kernel could read a disk image's blocks itself, but not safely while the Mac has
 * the volume mounted (the File Manager caches its catalog and bitmap in RAM), and the disk may be a
 * SCSI disk that only the Mac reaches. So the Mac's own File Manager does the work, called by the
 * Aurora68 Agent (mac/piagent, an INIT) at application time, and this file is the kernel's half: the
 * HTTP endpoints, the agent's request block in the card window (psmb.h, struct psmb_agent), and the
 * transfers, which go through the window's 24 KiB buffer a chunk at a time, so any size works.
 *
 * One job at a time (the agent does one request at a time); another request meanwhile gets 503 "another
 * file job", as a second screenshot does. A job is a small state machine moved on by psmac_files_poll()
 * and by the transport asking for bytes (psmac_files_produce) or giving them (psmac_files_body); it never
 * waits. Each request to the agent has a deadline, so a Mac that stops answering ends the job with an
 * error instead of a hung connection. Uploads are flow-controlled: the TCP window the kernel advertises
 * is what the job can take (psmac_files_room, psmac_ip.c), so a megabyte from the MacBook waits in the
 * MacBook's socket rather than in the Pi.
 *
 *   GET  /files/list?path=P            the entries of a folder (P "" or absent: the volumes), JSON
 *   GET  /files/info?path=P            one file or folder, JSON
 *   GET  /files/data?path=P[&fork=rsrc][&macbinary=1]
 *                                      the data fork (or the resource fork), or MacBinary II with both forks
 *                                      and the Finder information; X-Aurora-* headers carry the type, creator ...
 *   PUT  /files/data?path=P[&macbinary=1][&type=TEXT&creator=ttxt][&force=1][&mkdir=1]
 *                                      the body as the data fork, or a MacBinary file (forks, type, creator,
 *                                      flags and dates from it); P ending in ':' a folder (the MacBinary name)
 *   POST /files/mkdir?path=P[&parents=1]   POST /files/delete?path=P   POST /files/rename?path=P&to=Q
 *   GET  /files/progress               the transfer under way, or the last one (answered beside a job, never busy;
 *                                      23 November 2026, psmac_files_progress_json)
 *   GET  /apps                         the programs that run, JSON
 *   POST /apps/open?path=P             an application launched (or brought to the front), a document opened
 *   POST /apps/quit?name=N|creator=C   a 'quit' Apple event (System 7)
 * Paths are UTF-8 in the URL and Mac Roman on the Mac: "Volume:Folder:File", ":Folder" the startup volume.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_files.h"
#include "psmac_windows.h"

void sprintf(char *buf, const char *format, ...);                           /* support.c */

_Static_assert(sizeof(struct psmb_agent) == 0x520 && __builtin_offsetof(struct psmb_agent, path) == 0x80 &&
               __builtin_offsetof(struct psmb_agent, magic) == 0x380, "psmb_agent: the agent (mac/piagent/agent.c) checks the same");
_Static_assert(sizeof(struct psmb_agent_entry) == 80 && sizeof(struct psmb_agent_proc) == 72, "psmb_agent records");

static inline uint64_t now_ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t now_ms(void) { return (uint32_t)(now_ticks() / (tick_hz() / 1000u)); }
static uint64_t us_since(uint64_t t) { return t ? (now_ticks() - t) * 1000000u / tick_hz() : 0; }
const char *psmac_option(const char *key);                                  /* psmac_bus.c */
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int eq(const char *a, const char *b) { return !__builtin_strcmp(a, b); }
static void cpy(void *d, const volatile void *s, uint32_t n) { uint8_t *a = d; const volatile uint8_t *b = s; while (n--) *a++ = *b++; }
static void cpy_to(volatile void *d, const void *s, uint32_t n) { volatile uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void mb(void) { __asm__ volatile("dmb ish" ::: "memory"); }

static volatile struct psmb_agent *ag;
static volatile uint8_t *agbuf;

/* ---- is the agent there? ---- */
/* Its VBL task counts vbl_beat sixty times a second. Seen moving in the last 1.5 s: installed and the
   Mac's interrupts running. Installed but still: the Mac takes no interrupts (a system error, the debugger). */
static uint32_t beat_seen, beat_ms;
static int beat_known;
static void agent_sample(void)
{
    uint32_t b = ag->vbl_beat;
    if (!beat_known || b != beat_seen) { beat_seen = b; beat_ms = now_ms(); beat_known = 1; }
}
static int agent_installed(void) { return ag && ag->magic == PSMB_AGENT_MAGIC; }
/* 5 s: a launch under QEMU held the Mac's interrupts off for longer than the 1.5 s first allowed */
static int agent_alive(void) { return agent_installed() && now_ms() - beat_ms < 5000; }

/* ---- the job ---- */
enum { J_NONE, J_LIST, J_STAT, J_PULL, J_PUSH, J_MKDIR, J_DELETE, J_RENAME, J_OPEN, J_APPS, J_QUIT, J_FRONT };
static const char *const kind_name[] = { "", "list", "info", "pull", "push", "mkdir", "delete", "rename", "open", "apps", "quit", "front" };
#define FRONT_CONN 1000                          /* the job of psmac_files_front: the remote's own, no connection */
enum { S_WAIT_AGENT, S_GO, S_STREAM, S_HEAD, S_CREATE, S_BODY, S_SETINFO, S_REPLACE_DEL, S_REPLACE_REN, S_CLEAN, S_DONE };
#define OUT_SZ   (2 * PSMB_AGENT_BUF + 8192)     /* a chunk being sent, the next one, and headers */
/* An upload's body: 64 KiB, so that the next chunk comes in while the Mac writes the last, and so that the
   job never needs the last few bytes of the window it offered. Until 23 November 2026 this was one chunk
   (24 KiB), and a chunk was written only once it was whole. A Mac or BSD sender (the owner's MacBook) sends
   full segments only, and holds back a remainder shorter than a segment and shorter than half the largest
   window it has seen (silly-window avoidance) until its persist timer fires, five seconds at the least: 24,576
   bytes are 16 segments of 1460 and 1216 bytes, so every chunk waited five seconds for its last 1216 bytes
   (222,208 bytes in 71 s on the owner's SE, 4.8 KB/s in the testbed with pinet's BSD sender; docs/08 item 123).
   Now a chunk goes to the Mac as soon as 24 KiB are in, which leaves 40 KiB of window open, and anything in
   hand goes after T_PARTIAL without new bytes. */
#define JIN_SZ   (2 * PSMB_AGENT_BUF + 16384)
#define T_PARTIAL 150u                           /* ms without new bytes: what is in hand is written, a chunk or not */
#define ONE_JSON 1200                            /* the most one entry's JSON takes */
#define T_AGENT  3000u                           /* how long a job waits for an agent that is not there (yet) */
#define T_REQ    20000u                          /* how long one request may take: longer than a menu is usually held */
#define T_BODY   20000u                          /* an upload that sends nothing for this long has stopped (below, psmac_files_poll) */
static struct {
    int kind, conn, state;
    int posted; uint32_t seq, op, deadline, waited_from;
    int head_sent, done, broken;
    uint8_t path[256], path2[256];               /* Mac Roman */
    char upath[400];                             /* the path as given, JSON-safe, for the answer and the log */
    uint32_t flags, ftype, fcreator, fdflags, crdate, mddate, index;
    struct psmb_agent_entry info;
    /* pull and push: the fork under way and the position in it; the forks' lengths */
    int macbinary, fork; uint32_t pos, len[2], total;
    /* push: the body */
    int32_t clen; uint32_t body_got, skip, last_rx; int created, discard;
    int replacing; uint8_t target[256];          /* force=1: the file to replace, written once the upload is whole (start_replace) */
    uint32_t in_len;
    /* list, apps: the records of the last answer, turned into JSON as the connection takes it */
    uint32_t raw_n, raw_i, listed, more;
    /* the answer */
    uint32_t out_len, out_pos;
    uint32_t t0, bytes;
    uint32_t reqs, agent_ms;                     /* requests answered, and the time the agent took over them */
    /* where the time goes (23 November 2026, docs/08 item 123): per request, from posting to the agent's
       taken_seq (the Mac had not come round yet) and from there to its answer (the File Manager's work);
       for a transfer the time spent waiting for the network: an upload with less than a chunk in hand, a
       download whose answer buffer is full */
    uint64_t post_t, taken_t, net_t;
    uint64_t queue_us, work_us, net_us;
    uint32_t sent;                               /* answer bytes handed to the connection */
    int err_code; char err[200];
} J;
/* the job's buffers: the body coming in, the records of the last answer, the answer going out */
static uint8_t jin[JIN_SZ], jraw[PSMB_AGENT_BUF], jout[OUT_SZ];
static int trace;                                /* mac.filestrace=1: a log line per request of a transfer */
/* the last job's progress, kept once it has ended, for /files/progress */
static struct { int kind; char upath[400]; uint32_t done, total, ms; int ok; } lastp;
static char last[800];
static int front_res;                            /* psmac_files_front: 0 waiting, 1 done, 2 it was in front, -1 failed */
static uint8_t front_prev[32];                           /* the last job's outcome, for /state */
static uint32_t jobs, jobs_failed;
static volatile int mac_was_reset;

/* ---- the answer's bytes ---- */
static void out_compact(void)
{
    if (!J.out_pos) return;
    uint32_t n = J.out_len - J.out_pos;
    __builtin_memmove(jout, jout + J.out_pos, n);
    J.out_len = n; J.out_pos = 0;
}
static uint32_t out_free(void) { return OUT_SZ - (J.out_len - J.out_pos); }
static void out_add(const void *p, uint32_t n)
{
    if (J.out_len + n > OUT_SZ) out_compact();
    if (J.out_len + n > OUT_SZ) n = OUT_SZ - J.out_len;
    __builtin_memcpy(jout + J.out_len, p, n);
    J.out_len += n;
}
static void out_str(const char *s) { out_add(s, slen(s)); }
#define OUTF(...) do { char b_[1024]; sprintf(b_, __VA_ARGS__); out_str(b_); } while (0)

/* a Mac Roman string as UTF-8 inside a JSON string */
static void js(char *o, unsigned max, const uint8_t *mr, unsigned n) { psmac_macroman_utf8(mr, n, o, max, 1); }
static unsigned cl(const volatile char *s, unsigned max) { unsigned n = 0; while (n < max && s[n]) n++; return n; }
/* a four-character code as JSON text */
static void js4(char *o, uint32_t t)
{
    uint8_t b[4] = { (uint8_t)(t >> 24), (uint8_t)(t >> 16), (uint8_t)(t >> 8), (uint8_t)t };
    js(o, 32, b, 4);
}
/* seconds since 1904 (the Mac's clock, local time) as 2026-11-21T10:15:00 */
static void mac_date(char *o, uint32_t s)
{
    if (!s) { o[0] = 0; return; }
    int64_t days = s / 86400u - 24107;                     /* days from 1904 to 1970: 66 years, 17 of them leap */
    uint32_t r = s % 86400u;
    days += 719468;                                        /* civil_from_days (H. Hinnant) */
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    uint32_t doe = (uint32_t)(days - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    if (y > 2040) { o[0] = 0; return; }                    /* the Mac's clock ends in 2040; a larger number is no date */
    sprintf(o, "%04d-%02d-%02dT%02d:%02d:%02d", (int)y, (int)m, (int)d, (int)(r / 3600), (int)(r / 60 % 60), (int)(r % 60));
}
/* bytes for an HTTP header value: printable ASCII as it is, the rest %XX */
static void hdr_bytes(char *o, const uint8_t *b, unsigned n)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) {
        if (b[i] > 0x20 && b[i] < 0x7f && b[i] != '%') o[k++] = (char)b[i];
        else { o[k++] = '%'; o[k++] = hex[b[i] >> 4]; o[k++] = hex[b[i] & 15]; }
    }
    o[k] = 0;
}
/* UTF-8 bytes %XX-encoded, for a name in a header */
static void hdr_name(char *o, const uint8_t *mr, unsigned n)
{
    char u[100];                                           /* a name of 31 Mac Roman characters is 93 bytes of UTF-8 at most */
    unsigned k = psmac_macroman_utf8(mr, n, u, sizeof u, 0);
    hdr_bytes(o, (const uint8_t *)u, k);
}

/* ---- errors: the Mac's, the agent's, ours ---- */
static int err_http(int32_t st, const char **text)
{
    static const struct { int16_t e; int16_t code; const char *text; } t[] = {
        { -43, 404, "not found" }, { -120, 404, "a folder on the way is not there (push --mkdir or mkdir -p make it)" },
        { -35, 404, "no such volume" }, { -48, 409, "it is there already" }, { -47, 409, "busy: the file is open" },
        { -49, 409, "the file is open for writing by another program" }, { -45, 403, "the file is locked" },
        { -46, 403, "the volume is locked" }, { -44, 403, "the disk is write-protected" },
        { -54, 403, "no permission (the file is open or locked)" }, { -61, 403, "no permission to write" },
        { -34, 507, "the disk is full" }, { -33, 507, "the folder is full" }, { -37, 400, "a bad name" },
        { -50, 400, "a bad parameter" }, { -108, 503, "the Mac ran out of memory" }, { -600, 404, "no such program" },
        { -903, 409, "the program does not take Apple events" }, { -906, 409, "the program does not take Apple events" }, { -609, 409, "the program has gone" },
        { -1712, 504, "the program did not answer the Apple event" }, { -23, 409, "the file is already open" },
        { -36, 500, "an I/O error on the disk" }, { -1302, 409, "a folder, not a file" },
        { PSMB_AGE_NOTEMPTY, 409, "the folder is not empty (rm takes empty folders only)" },
        { PSMB_AGE_ISDIR, 409, "it is a folder" }, { PSMB_AGE_NOTDIR, 409, "it is a file, not a folder" },
        { PSMB_AGE_UNSUPPORTED, 501, "not on this system" }, { PSMB_AGE_XVOLUME, 400, "mv stays on one volume" },
        { PSMB_AGE_BADPATH, 400, "a bad path (Volume:Folder:File, names of 31 characters at most, no empty parts)" },
        { PSMB_AGE_NOAPP, 404, "no application for this document" }, { PSMB_AGE_NOPROC, 404, "no such program running" },
        { PSMB_AGE_BADOP, 500, "the agent does not know this request (an older agent)" },
        { PSMB_AGE_BUSY, 503, "the Mac is not ready for it yet" },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].e == st) { *text = t[i].text; return t[i].code; }
    *text = 0;
    return 500;
}
static const char *http_text(int code)
{
    switch (code) {
    case 400: return "Bad Request"; case 403: return "Forbidden"; case 408: return "Request Timeout"; case 404: return "Not Found"; case 409: return "Conflict";
    case 411: return "Length Required"; case 501: return "Not Implemented"; case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout"; case 507: return "Insufficient Storage"; default: return "Internal Server Error";
    }
}
/* the bytes of the forks a transfer moves (J.bytes counts them as the Mac writes or reads them) */
static uint32_t fork_total(void)
{
    if (J.kind == J_PULL && !J.macbinary) return J.len[J.fork > 1 ? 1 : J.fork];
    return J.kind == J_PUSH || J.kind == J_PULL ? J.len[0] + J.len[1] : 0;
}
static void finish_log(int ok)
{
    jobs++;
    if (!ok) jobs_failed++;
    uint32_t ms = now_ms() - J.t0;
    if (ok && (J.kind == J_PUSH || J.kind == J_PULL)) {
        if (J.net_t) { J.net_us += us_since(J.net_t); J.net_t = 0; }
        sprintf(last, "%s %s: done, %u bytes in %u ms, %u KB/s (%u requests to the agent: %u ms waiting for the Mac to come round, "
                      "%u ms of its work; %u ms waiting for the network)", kind_name[J.kind], J.upath, J.bytes, ms,
                (uint32_t)((uint64_t)J.bytes * 1000u / 1024u / (ms ? ms : 1)), J.reqs, (uint32_t)(J.queue_us / 1000u),
                (uint32_t)(J.work_us / 1000u), (uint32_t)(J.net_us / 1000u));
    }
    else if (ok) sprintf(last, "%s %s: done, %u bytes in %u ms (%u requests to the agent, %u ms of it waiting for its answers)", kind_name[J.kind], J.upath, J.bytes, ms, J.reqs, J.agent_ms);
    else sprintf(last, "%s %s: %s", kind_name[J.kind], J.upath, J.err);
    kprintf("[FILES] %s\n", last);
    lastp.kind = J.kind; __builtin_memcpy(lastp.upath, J.upath, sizeof lastp.upath);
    lastp.done = J.bytes; lastp.total = fork_total(); lastp.ms = ms; lastp.ok = ok;
}
/* the job's answer is an error: as JSON while nothing has been sent, else the connection is cut short
   (the client sees fewer bytes than Content-Length said; /state's "last" says why) */
static void respond_error_now(void)
{
    if (!J.head_sent) {
        char m[300], b[800];
        psmac_macroman_utf8((const uint8_t *)J.err, slen(J.err), m, sizeof m, 1);
        sprintf(b, "{\"error\": \"%s\", \"path\": \"%s\"}\n", m, J.upath);
        J.out_len = J.out_pos = 0;
        OUTF("HTTP/1.0 %d %s\r\nServer: Aurora68\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: application/json; charset=utf-8\r\n"
             "Content-Length: %u\r\n\r\n", J.err_code, http_text(J.err_code), slen(b));
        out_str(b);
        J.head_sent = 1;
    } else J.broken = 1;
    J.done = 1; J.state = S_DONE;
    finish_log(0);
}
static void fail(int code, const char *msg)
{
    if (J.state == S_DONE) return;
    J.err_code = code;
    unsigned n = 0;
    for (; msg[n] && n < sizeof J.err - 1; n++) J.err[n] = msg[n];
    J.err[n] = 0;
    /* a push that made a new file and did not finish: the partial file goes first */
    if (J.kind == J_PUSH && J.created == 1) { J.created = 2; J.state = S_CLEAN; J.discard = 1; return; }
    if (J.kind == J_PUSH) J.discard = 1;
    respond_error_now();
}
/* the agent's status as an error: its message, and its note if it wrote one */
static void fail_status(int32_t st)
{
    const char *t;
    int code = err_http(st, &t);
    char m[200], note[70];
    unsigned k = cl(ag->note, 63);
    cpy(note, ag->note, k); note[k] = 0;
    if (t) sprintf(m, "%s%s%s%s", t, k ? " (" : "", note, k ? ")" : "");
    else sprintf(m, "the Mac's error %d%s%s%s", (int)st, k ? " (" : "", note, k ? ")" : "");
    fail(code, m);
}

/* ---- one request to the agent ---- */
static void post(uint32_t op, uint32_t timeout_ms)
{
    volatile struct psmb_agent *a = ag;
    a->op = op; a->flags = J.flags; a->index = J.index;
    a->ftype = J.ftype; a->fcreator = J.fcreator; a->fdflags = J.fdflags; a->crdate = J.crdate; a->mddate = J.mddate;
    cpy_to(a->path, J.path, 256); cpy_to(a->path2, J.path2, 256);
    mb();                                                  /* the fields before the sequence number that publishes them */
    J.seq = a->req_seq + 1; J.op = op;
    a->req_seq = J.seq;
    J.posted = 1; J.deadline = now_ms() + timeout_ms; J.waited_from = now_ms();
    J.post_t = now_ticks(); J.taken_t = 0;
}
static void post_io(uint32_t op, int fork, uint32_t offset, uint32_t length)
{
    ag->fork = (uint32_t)fork; ag->offset = offset; ag->length = length;
    post(op, T_REQ);
}

/* ---- force=1: the old file replaced only once the new one is whole (21 November 2026) ----
   The first version emptied the existing file at the start (CREATE with PSMB_AGF_FORCE) and wrote into it.
   An upload that broke off then left the Mac with half a file: on the owner's SE a push of Riptide over
   WiFi stalled, and the Riptide extension in the System Folder was no longer whole (docs/08 item 95). Now
   the upload goes into a new file beside it ("name.part", made like any new file, so a broken upload
   deletes it again), and only after its information is set is the old file deleted and the new one
   renamed into its place. If the old file cannot be deleted (in use), the new one stays beside it and the
   answer says so. */
static void start_replace(void)
{
    unsigned n = slen((const char *)J.path), cut = n;
    while (cut && J.path[cut - 1] != ':') cut--;               /* the leaf starts at cut */
    unsigned leaf = n - cut, keep = leaf > 26 ? 26 : leaf;     /* name.part within HFS's 31 characters */
    if (cut + keep + 5 > 255) return;                          /* too long to make: the old way, in place */
    __builtin_memcpy(J.target, J.path, n + 1);
    __builtin_memcpy(J.path + cut + keep, ".part", 6);
    J.replacing = 1;
}

/* ---- JSON of the records ---- */
static void entry_json(const struct psmb_agent_entry *e, int with_path)
{
    char name[200], ty[32], cr[32], c[24], m[24];
    js(name, sizeof name, (const uint8_t *)e->name, cl(e->name, 31));
    mac_date(c, e->crdate); mac_date(m, e->mddate);
    if (with_path) OUTF("{\"path\": \"%s\", ", J.upath); else out_str("{");
    if (e->flags & PSMB_AGN_VOLUME) {
        OUTF("\"name\": \"%s\", \"kind\": \"volume\", \"free_kb\": %u, \"size_kb\": %u, \"startup\": %s, \"locked\": %s, \"created\": \"%s\", \"modified\": \"%s\", \"vrefnum\": %d}",
             name, e->dlen, e->rlen, e->flags & PSMB_AGN_STARTUP ? "true" : "false", e->flags & PSMB_AGN_LOCKED ? "true" : "false", c, m, (int)(int32_t)e->id);
        return;
    }
    if (e->flags & PSMB_AGN_DIR) {
        OUTF("\"name\": \"%s\", \"kind\": \"folder\", \"entries\": %u, \"created\": \"%s\", \"modified\": \"%s\", \"id\": %u}", name, e->dlen, c, m, e->id);
        return;
    }
    js4(ty, e->type); js4(cr, e->creator);
    OUTF("\"name\": \"%s\", \"kind\": \"file\", \"type\": \"%s\", \"creator\": \"%s\", \"data\": %u, \"rsrc\": %u, \"created\": \"%s\", \"modified\": \"%s\", "
         "\"flags\": %u, \"locked\": %s, \"open\": %s, \"id\": %u}",
         name, ty, cr, e->dlen, e->rlen, c, m, e->fdflags & 0xffff, e->flags & PSMB_AGN_LOCKED ? "true" : "false", e->flags & PSMB_AGN_OPEN ? "true" : "false", e->id);
}
static void proc_json(const struct psmb_agent_proc *p)
{
    char name[200], ty[32], cr[32];
    js(name, sizeof name, (const uint8_t *)p->name, cl(p->name, 31));
    js4(ty, p->type); js4(cr, p->creator);
    if (p->flags & PSMB_AGP_CURRENT) { OUTF("{\"name\": \"%s\", \"front\": true, \"note\": \"System 6: the current application only\"}", name); return; }
    OUTF("{\"name\": \"%s\", \"type\": \"%s\", \"creator\": \"%s\", \"psn\": \"%u.%u\", \"front\": %s, \"background_only\": %s, \"desk_accessory\": %s, "
         "\"apple_events\": %s, \"size\": %u, \"free\": %u}",
         name, ty, cr, p->psn_hi, p->psn_lo, p->flags & PSMB_AGP_FRONT ? "true" : "false", p->mode & 0x400 ? "true" : "false",
         p->mode & 0x20000 ? "true" : "false", p->mode & 0x40 ? "true" : "false", p->size, p->free);
}
static void json_head(void)
{
    out_str("HTTP/1.0 200 OK\r\nServer: Aurora68\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: application/json; charset=utf-8\r\n\r\n");
    J.head_sent = 1;
}
/* a one-line JSON answer and the end of the job */
static void answer(const char *json)
{
    json_head();
    out_str(json);
    J.done = 1; J.state = S_DONE;
    finish_log(1);
}

/* ---- MacBinary II (the format of the tools/mac/rsrc.py packager): a 128-byte header, the forks padded to 128 ---- */
static uint16_t crc_xmodem(const uint8_t *p, unsigned n)
{
    uint16_t c = 0;
    while (n--) { c ^= (uint16_t)(*p++ << 8); for (int k = 0; k < 8; k++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1); }
    return c;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t pad128(uint32_t n) { return (n + 127) & ~127u; }
static void macbinary_header(uint8_t *h, const struct psmb_agent_entry *e)
{
    for (int i = 0; i < 128; i++) h[i] = 0;
    unsigned n = cl(e->name, 31);
    h[1] = (uint8_t)n;
    for (unsigned i = 0; i < n; i++) h[2 + i] = (uint8_t)e->name[i];
    put32(h + 65, e->type); put32(h + 69, e->creator);
    h[73] = (uint8_t)(e->fdflags >> 8);
    h[75] = (uint8_t)(e->location >> 24); h[76] = (uint8_t)(e->location >> 16);   /* vertical, then horizontal */
    h[77] = (uint8_t)(e->location >> 8); h[78] = (uint8_t)e->location;
    h[81] = e->flags & PSMB_AGN_LOCKED ? 1 : 0;
    put32(h + 83, e->dlen); put32(h + 87, e->rlen);
    put32(h + 91, e->crdate); put32(h + 95, e->mddate);
    h[101] = (uint8_t)e->fdflags;
    h[122] = 129; h[123] = 129;                            /* written by MacBinary II, readable by II */
    uint16_t c = crc_xmodem(h, 124);
    h[124] = (uint8_t)(c >> 8); h[125] = (uint8_t)c;
}
/* a MacBinary header as an upload's first 128 bytes: the forks' lengths, type, creator, flags, dates */
static int macbinary_parse(const uint8_t *h, char *why)
{
    if (h[0] || h[74] || h[1] < 1 || h[1] > 63) { sprintf(why, "not a MacBinary file (its first bytes are not a MacBinary header)"); return 0; }
    if (h[122] >= 129 && ((uint16_t)(h[124] << 8 | h[125])) != crc_xmodem(h, 124)) { sprintf(why, "the MacBinary header's CRC is wrong"); return 0; }
    if (h[122] < 129 && h[82]) { sprintf(why, "not a MacBinary file (byte 82 of a MacBinary I header is 0)"); return 0; }
    J.ftype = get32(h + 65); J.fcreator = get32(h + 69);
    J.fdflags = (uint32_t)h[73] << 8 | (h[122] >= 129 ? h[101] : 0);
    J.len[0] = get32(h + 83); J.len[1] = get32(h + 87);
    J.crdate = get32(h + 91); J.mddate = get32(h + 95);
    uint32_t sec = h[122] >= 129 ? (uint32_t)(h[120] << 8 | h[121]) : 0;
    J.skip = pad128(sec);                                  /* a secondary header, which nobody writes, padded */
    uint32_t need = 128 + J.skip + pad128(J.len[0]) + J.len[1];
    if (J.clen >= 0 && (uint32_t)J.clen < need) { sprintf(why, "the MacBinary file is %u bytes, its header says at least %u", (uint32_t)J.clen, need); return 0; }
    if (J.path[0] && J.path[slen((const char *)J.path) - 1] == ':') {   /* into a folder: the file's own name */
        unsigned p = slen((const char *)J.path), n = h[1] > 31 ? 31 : h[1];
        for (unsigned i = 0; i < n && p < 255; i++) J.path[p++] = h[2 + i];
        J.path[p] = 0;
        js(J.upath, sizeof J.upath, J.path, p);
    }
    return 1;
}

/* ---- the job moves on ---- */
static void reply(void);
static void step(void)
{
    switch (J.kind) {
    case J_LIST: case J_APPS:
        if (J.state == S_GO) { post(J.kind == J_LIST ? PSMB_AG_LIST : PSMB_AG_APPS, T_REQ); return; }
        if (J.state == S_STREAM) {
            uint32_t rec = J.kind == J_LIST ? sizeof(struct psmb_agent_entry) : sizeof(struct psmb_agent_proc);
            while (J.raw_i < J.raw_n && out_free() > ONE_JSON) {
                out_str(J.listed ? ",\n  " : "\n  ");
                if (J.kind == J_LIST) entry_json((const struct psmb_agent_entry *)(jraw + rec * J.raw_i), 0);
                else proc_json((const struct psmb_agent_proc *)(jraw + rec * J.raw_i));
                J.raw_i++; J.listed++;
            }
            if (J.raw_i < J.raw_n) return;
            if (J.more && J.raw_n) { J.index += J.raw_n; J.state = S_GO; post(J.kind == J_LIST ? PSMB_AG_LIST : PSMB_AG_APPS, T_REQ); return; }
            if (out_free() < 64) return;
            OUTF("\n], \"count\": %u}\n", J.listed);
            J.done = 1; J.state = S_DONE; J.bytes = J.listed;
            finish_log(1);
        }
        return;
    case J_STAT: case J_PULL:
        if (J.state == S_GO) { post(PSMB_AG_STAT, T_REQ); return; }
        if (J.state != S_STREAM) return;
        for (;;) {                                         /* the next chunk of a fork, while the answer has room for it */
            if (J.fork > 1 || (!J.macbinary && J.pos >= J.len[J.fork])) {
                J.done = 1; J.state = S_DONE; finish_log(1); return;
            }
            if (J.pos >= J.len[J.fork]) {                  /* MacBinary: the fork's padding, then the next fork */
                static const uint8_t zero[128];
                uint32_t pad = pad128(J.len[J.fork]) - J.len[J.fork];
                if (out_free() < pad) return;
                out_add(zero, pad);
                J.fork++; J.pos = 0;
                continue;
            }
            uint32_t n = J.len[J.fork] - J.pos;
            if (n > PSMB_AGENT_BUF) n = PSMB_AGENT_BUF;
            if (out_free() < n + 128) { if (!J.net_t) J.net_t = now_ticks(); return; }   /* the network's turn */
            if (J.net_t) { J.net_us += us_since(J.net_t); J.net_t = 0; }
            if (trace) kprintf("[FILES]   read %s %u+%u (%u waiting to be sent)\n", J.fork ? "rsrc" : "data", J.pos, n, J.out_len - J.out_pos);
            post_io(PSMB_AG_READ, J.fork, J.pos, n);
            return;
        }
    case J_PUSH:
        if (J.state == S_HEAD) {
            if (!J.macbinary) { J.len[0] = (uint32_t)J.clen; J.len[1] = 0; J.state = S_CREATE; }
            else {
                if (J.in_len < 128) {
                    if (J.clen >= 0 && J.body_got >= (uint32_t)J.clen) fail(400, "the body is shorter than a MacBinary header");
                    return;
                }
                char why[120];
                if (!macbinary_parse(jin, why)) { fail(400, why); return; }
                __builtin_memmove(jin, jin + 128, J.in_len - 128); J.in_len -= 128;
                J.state = S_CREATE;
            }
            J.total = J.len[0] + J.len[1];
        }
        if (J.state == S_CREATE) {
            if ((J.flags & PSMB_AGF_FORCE) && !J.replacing) start_replace();   /* a leftover .part is emptied: FORCE stays */
            post(PSMB_AG_CREATE, T_REQ);
            return;
        }
        if (J.state == S_REPLACE_DEL) {                    /* the new file is whole: the old one goes */
            uint8_t part[256]; __builtin_memcpy(part, J.path, sizeof part);
            __builtin_memcpy(J.path, J.target, sizeof J.path); J.flags = 0;
            post(PSMB_AG_DELETE, T_REQ);
            __builtin_memcpy(J.path, part, sizeof J.path);  /* J.path stays the new file's (a cleanup deletes it, not the old) */
            return;
        }
        if (J.state == S_REPLACE_REN) { __builtin_memcpy(J.path2, J.target, sizeof J.path2); J.flags = 0; post(PSMB_AG_RENAME, T_REQ); return; }
        if (J.state == S_BODY) {
            for (;;) {
                if (J.skip) {                              /* padding and headers between the forks */
                    uint32_t k = J.skip < J.in_len ? J.skip : J.in_len;
                    __builtin_memmove(jin, jin + k, J.in_len - k); J.in_len -= k; J.skip -= k;
                    if (J.skip) break;
                }
                if (J.pos >= J.len[J.fork]) {
                    if (J.fork == 0 && J.macbinary) J.skip = pad128(J.len[0]) - J.len[0];
                    if (++J.fork > 1) { J.state = S_SETINFO; post(PSMB_AG_SETINFO, T_REQ); return; }
                    J.pos = 0;
                    continue;
                }
                uint32_t n = J.len[J.fork] - J.pos;
                if (n > PSMB_AGENT_BUF) n = PSMB_AGENT_BUF;
                if (J.in_len < n) {                        /* a whole chunk, or the rest of the fork */
                    if (J.clen >= 0 && J.body_got >= (uint32_t)J.clen) { fail(400, "the body ended before the file did"); break; }
                    if (!J.in_len || now_ms() - J.last_rx < T_PARTIAL) {   /* the network's turn */
                        if (!J.net_t) J.net_t = now_ticks();
                        break;
                    }
                    n = J.in_len;                          /* nothing more for a while: what is here (above, JIN_SZ) */
                }
                if (J.net_t) { J.net_us += us_since(J.net_t); J.net_t = 0; }
                if (trace) kprintf("[FILES]   write %s %u+%u (%u in hand, %u of %d received)\n", J.fork ? "rsrc" : "data", J.pos, n, J.in_len, J.body_got, (int)J.clen);
                cpy_to(agbuf, jin, n);
                __builtin_memmove(jin, jin + n, J.in_len - n); J.in_len -= n;
                post_io(PSMB_AG_WRITE, J.fork, J.pos, n);
                return;
            }
            return;
        }
        if (J.state == S_CLEAN) { J.flags = 0; post(PSMB_AG_DELETE, T_REQ); return; }
        return;
    case J_MKDIR: post(PSMB_AG_MKDIR, T_REQ); return;
    case J_DELETE: post(PSMB_AG_DELETE, T_REQ); return;
    case J_RENAME: post(PSMB_AG_RENAME, T_REQ); return;
    case J_OPEN: post(PSMB_AG_OPEN, 30000); return;        /* a launch reads the program and its resources: give it longer */
    case J_QUIT: post(PSMB_AG_QUIT, T_REQ); return;
    }
}

/* the agent has answered the request J.seq */
static void reply(void)
{
    volatile struct psmb_agent *a = ag;
    mb();
    int32_t st = a->status;
    uint32_t res = a->result;
    J.posted = 0;
    J.reqs++; J.agent_ms += now_ms() - J.waited_from;
    {   /* the request's time: until the agent took it (its taken_seq; an older agent has none: all of it its work), then its work */
        uint64_t all = us_since(J.post_t), q = J.taken_t ? (J.taken_t - J.post_t) * 1000000u / tick_hz() : 0;
        if (q > all) q = all;
        J.queue_us += q; J.work_us += all - q;
        if (trace && (J.kind == J_PUSH || J.kind == J_PULL))
            kprintf("[FILES]   op %u answered: %u us until the Mac took it, %u us of its work\n", J.op, (uint32_t)q, (uint32_t)(all - q));
    }
    if (J.state == S_CLEAN) {                              /* the partial file is gone (or could not go): now the error */
        if (st) { unsigned n = slen(J.err); sprintf(J.err + n, "; the partial file could not be deleted (%d)", (int)st); }
        else { unsigned n = slen(J.err); sprintf(J.err + n, "; the partial file was deleted"); }
        if (J.conn < 0) { J.state = S_DONE; J.done = 1; finish_log(0); return; }
        respond_error_now();
        return;
    }
    if (st && J.kind == J_PUSH && J.state == S_REPLACE_DEL && st == -43) { J.state = S_REPLACE_REN; return; }   /* no old file after all */
    if (st && J.kind == J_PUSH && (J.state == S_REPLACE_DEL || J.state == S_REPLACE_REN)) {
        J.created = 0;                                    /* the new file is whole: it stays */
        char m[200], t[300]; js(t, sizeof t, J.path, slen((const char *)J.path));
        sprintf(m, "the upload is complete as \"%s\", but the old file could not be %s (the Mac's error %d%s): replace it by hand",
                t, J.state == S_REPLACE_DEL ? "deleted" : "replaced", (int)st, st == -47 ? ", the file is in use" :
                st == PSMB_AGE_NOTEMPTY || st == PSMB_AGE_ISDIR ? ": that name is a folder" : "");
        fail(409, m);
        return;
    }
    if (st) { fail_status(st); return; }
    char b[1200];
    switch (J.kind) {
    case J_LIST: case J_APPS:
        if (res > PSMB_AGENT_BUF / (J.kind == J_LIST ? sizeof(struct psmb_agent_entry) : sizeof(struct psmb_agent_proc))) { fail(500, "the agent's answer does not fit its buffer"); return; }
        cpy(jraw, agbuf, res * (J.kind == J_LIST ? sizeof(struct psmb_agent_entry) : sizeof(struct psmb_agent_proc)));
        J.raw_n = res; J.raw_i = 0; J.more = a->more;
        if (!J.head_sent) {
            json_head();
            if (J.kind == J_LIST) OUTF("{\"path\": \"%s\", \"entries\": [", J.upath);
            else OUTF("{\"system\": \"%u.%u\", \"apps\": [", (a->sysversion >> 8) & 15, (a->sysversion >> 4) & 15);
        }
        J.state = S_STREAM;
        return;
    case J_STAT: case J_PULL:
        cpy(&J.info, agbuf, sizeof J.info);
        if (J.kind == J_STAT) {
            json_head();
            entry_json(&J.info, 1);
            out_str("\n");
            J.done = 1; J.state = S_DONE; finish_log(1);
            return;
        }
        if (J.state == S_GO) {                             /* the file's information: the answer's headers */
            if (J.info.flags & PSMB_AGN_DIR) { fail(409, "it is a folder (ls lists it)"); return; }
            J.len[0] = J.info.dlen; J.len[1] = J.info.rlen;
            if (J.macbinary) { J.total = 128 + pad128(J.len[0]) + pad128(J.len[1]); J.fork = 0; }
            else { J.total = J.len[J.fork]; if (J.fork == 1) J.len[0] = 0; }
            char nm[300], ty[16], cr[16], c[24], m[24];
            hdr_name(nm, (const uint8_t *)J.info.name, cl(J.info.name, 31));
            uint8_t t4[4], c4[4]; put32(t4, J.info.type); put32(c4, J.info.creator);
            hdr_bytes(ty, t4, 4); hdr_bytes(cr, c4, 4);
            mac_date(c, J.info.crdate); mac_date(m, J.info.mddate);
            OUTF("HTTP/1.0 200 OK\r\nServer: Aurora68\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: %s\r\nContent-Length: %u\r\n",
                 J.macbinary ? "application/x-macbinary" : "application/octet-stream", J.total);
            OUTF("X-Aurora-Name: %s\r\nX-Aurora-Type: %s\r\nX-Aurora-Creator: %s\r\nX-Aurora-Flags: %u\r\nX-Aurora-Data-Length: %u\r\nX-Aurora-Rsrc-Length: %u\r\n"
                 "X-Aurora-Created: %s\r\nX-Aurora-Modified: %s\r\nX-Aurora-Format: %s\r\n\r\n",
                 nm, ty, cr, J.info.fdflags & 0xffff, J.info.dlen, J.info.rlen, c, m, J.macbinary ? "macbinary" : J.fork ? "rsrc" : "data");
            J.head_sent = 1;
            if (J.macbinary) { uint8_t h[128]; macbinary_header(h, &J.info); out_add(h, 128); }
            J.pos = 0;
            J.state = S_STREAM;
            return;
        }
        {   /* a chunk read */
            uint32_t asked = a->length;
            if (res > asked) res = asked;
            uint8_t *dst = jout + J.out_len;
            if (J.out_len + asked > OUT_SZ) { out_compact(); dst = jout + J.out_len; }
            cpy(dst, agbuf, res);
            J.out_len += res; J.pos += res; J.bytes += res;
            if (res < asked) {                             /* the file got shorter while it was read: the length promised is kept */
                kprintf("[FILES] %s: the file has %u bytes fewer than when the transfer began; zeros sent in their place\n", J.upath, asked - res);
                for (uint32_t i = res; i < asked; i++) jout[J.out_len++] = 0;
                J.pos = J.len[J.fork];
            }
            return;
        }
    case J_PUSH:
        if (J.state == S_CREATE) { J.created = res || J.replacing ? 1 : 0; J.state = S_BODY; J.fork = 0; J.pos = 0; return; }
        if (J.state == S_BODY) {
            if (res != a->length) { fail(507, "the disk took fewer bytes than were written (full?)"); return; }
            J.pos += res; J.bytes += res;
            return;
        }
        if (J.state == S_SETINFO && J.replacing) { J.state = S_REPLACE_DEL; return; }
        if (J.state == S_REPLACE_DEL) { J.state = S_REPLACE_REN; return; }
        if (J.state == S_SETINFO || J.state == S_REPLACE_REN) {
            char ty[32], cr[32];
            js4(ty, J.ftype); js4(cr, J.fcreator);
            J.created = 0;
            sprintf(b, "{\"path\": \"%s\", \"type\": \"%s\", \"creator\": \"%s\", \"data\": %u, \"rsrc\": %u, \"ms\": %u}\n",
                    J.upath, ty, cr, J.len[0], J.len[1], now_ms() - J.t0);
            answer(b);
        }
        return;
    case J_MKDIR: sprintf(b, "{\"path\": \"%s\", \"made\": true, \"id\": %u}\n", J.upath, res); answer(b); return;
    case J_DELETE: sprintf(b, "{\"path\": \"%s\", \"deleted\": true}\n", J.upath); answer(b); return;
    case J_RENAME: {
        char to[300]; js(to, sizeof to, J.path2, slen((const char *)J.path2));
        sprintf(b, "{\"path\": \"%s\", \"to\": \"%s\", \"done\": true}\n", J.upath, to); answer(b); return; }
    case J_OPEN: {
        char note[140], n[70]; unsigned k = cl(a->note, 63); cpy(n, a->note, k); n[k] = 0;
        js(note, sizeof note, (const uint8_t *)n, k);
        sprintf(b, "{\"path\": \"%s\", \"opened\": true, \"how\": \"%s\", \"running_before\": %s}\n", J.upath,
                k ? note : res == 2 ? "brought to the front" : "launched", res == 2 ? "true" : "false");
        answer(b); return; }
    case J_QUIT: {
        char nm[140], n[70]; unsigned k = cl(a->note, 31); cpy(n, a->note, k); n[k] = 0;
        js(nm, sizeof nm, (const uint8_t *)n, k);
        sprintf(b, "{\"quit\": \"%s\", \"sent\": true, \"note\": \"a 'quit' Apple event; the program may ask to save first\"}\n", nm);
        answer(b); return; }
    }
}

static void job_end(void)
{
    J.kind = J_NONE; J.conn = -1; J.state = S_DONE; J.posted = 0; J.done = 0;
}

void psmac_files_poll(void)
{
    if (!ag) return;
    agent_sample();
    if (mac_was_reset) {
        mac_was_reset = 0;
        if (J.kind == J_FRONT) { front_res = -1; job_end(); }
        else if (J.kind != J_NONE && J.state != S_DONE) {
            J.created = 0;                                 /* nobody is left to delete a partial file */
            J.posted = 0;
            fail(503, "the Mac restarted during the job");
        }
    }
    if (J.kind == J_NONE) return;
    if (J.state == S_DONE) {                               /* everything sent, or nobody left to send it to */
        if (J.conn < 0 || (J.done && J.out_pos >= J.out_len && J.broken)) job_end();
        return;
    }
    if (J.posted) {
        if (!J.taken_t && ag->taken_seq == J.seq) J.taken_t = now_ticks();
        if (ag->done_seq == J.seq) {
            if (J.kind == J_FRONT) {                       /* the remote's request: its answer kept for psmac_files_front_result */
                mb();
                int32_t st = ag->status;
                unsigned k = cl(ag->note, 31);
                cpy(front_prev, ag->note, k); front_prev[k] = 0;
                front_res = st ? -1 : ag->result == 2 ? 2 : 1;
                if (st) kprintf("[FILES] front %s: the agent answered %d\n", J.upath, (int)st);
                J.posted = 0; job_end();
                return;
            }
            if (J.conn < 0 && J.state != S_CLEAN) { J.posted = 0; job_end(); return; }   /* nobody to tell */
            reply();
        }
        else if ((int32_t)(now_ms() - J.deadline) >= 0) {
            if (J.kind == J_FRONT) { front_res = -1; J.posted = 0; job_end(); return; }
            char m[200];
            int alive = agent_alive();
            sprintf(m, "the Aurora68 Agent did not answer within %u s: %s", (now_ms() - J.waited_from) / 1000,
                    alive ? "the Mac is busy (a menu held down, a modal dialog, a long disk operation) or the program in front takes no events"
                          : "the Mac has stopped taking interrupts (a system error, the debugger, a long time with interrupts off)");
            J.posted = 0;
            if (J.state == S_CLEAN) { J.created = 0; respond_error_now(); }
            else { if (J.kind == J_PUSH && J.created == 1) J.created = 0; fail(504, m); }
            return;
        }
        if (J.posted) return;
    }
    if (J.state == S_WAIT_AGENT) {
        if (agent_alive()) {
            J.state = J.kind == J_PUSH ? S_HEAD : S_GO;
        } else if ((int32_t)(now_ms() - J.deadline) >= 0) {
            fail(503, agent_installed() ? "the Aurora68 Agent is installed but the Mac is not running (a system error, the debugger, a restart)"
                                        : "the Aurora68 Agent is not running on the Mac: put it into the System Folder (System 6) or "
                                          "System Folder:Extensions (System 7) and restart the Mac (docs/13, \"Files and applications\")");
            return;
        } else return;
    }
    if (J.state == S_DONE) return;
    if (J.conn < 0 && J.state != S_CLEAN) { job_end(); return; }
    /* An upload whose bytes stopped coming: until 21 November 2026 nothing ended it, and a stalled push held
       the agent, refusing every later job ("another file job"), until the Pi restarted (docs/08 item 95). */
    if (J.kind == J_PUSH && (J.state == S_HEAD || J.state == S_BODY) && !J.discard && J.clen >= 0 &&
        J.body_got < (uint32_t)J.clen && now_ms() - J.last_rx > T_BODY) {
        char m[160];
        sprintf(m, "the upload stopped arriving: %u of %d bytes in, nothing more for %u s", J.body_got, (int)J.clen, T_BODY / 1000);
        fail(408, m);
        return;
    }
    step();
}
int psmac_files_busy(void) { return J.kind != J_NONE; }

/* The remote's /screenshot?front=1 (psmac_remote.c): an application brought to the front by the agent.
   1 asked (psmac_files_front_result then says how it went), 0 not possible now: no agent, System 6 (no
   Process Manager), or a file job under way. */
int psmac_files_front(const uint8_t *name, unsigned len)
{
    if (!agent_alive() || !(ag->features & PSMB_AGX_PROCMGR) || J.kind != J_NONE || !len || len > 31) return 0;
    __builtin_memset(&J, 0, sizeof J);
    __builtin_memcpy(J.path, name, len); J.path[len] = 0;
    js(J.upath, sizeof J.upath, J.path, len);
    J.kind = J_FRONT; J.conn = FRONT_CONN; J.state = S_GO; J.t0 = now_ms();
    front_res = 0; front_prev[0] = 0;
    post(PSMB_AG_FRONT, 3000);
    return 1;
}
int psmac_files_front_result(uint8_t *prev, unsigned max)
{
    if (front_res && prev && max) { unsigned n = slen((const char *)front_prev); if (n >= max) n = max - 1; __builtin_memcpy(prev, front_prev, n); prev[n] = 0; }
    return front_res;
}

/* ---- the request ---- */
/* a query value, %XX-decoded ('+' a space, as in a form) */
static int qget(const char *q, const char *key, char *out, unsigned max)
{
    unsigned kl = slen(key);
    for (const char *s = q; s && *s; ) {
        if (!__builtin_strncmp(s, key, kl) && s[kl] == '=') {
            unsigned n = 0;
            for (s += kl + 1; *s && *s != '&' && n + 1 < max; s++) {
                int c = (uint8_t)*s;
                if (c == '+') c = ' ';
                else if (c == '%' && s[1] && s[2]) {
                    int h = 0, ok = 1;
                    for (int i = 1; i <= 2; i++) {
                        char x = s[i];
                        h = h * 16 + (x >= '0' && x <= '9' ? x - '0' : x >= 'a' && x <= 'f' ? x - 'a' + 10 : x >= 'A' && x <= 'F' ? x - 'A' + 10 : (ok = 0));
                    }
                    if (ok) { c = h; s += 2; }
                }
                out[n++] = (char)c;
            }
            out[n] = 0;
            return 1;
        }
        while (*s && *s != '&') s++;
        if (*s) s++;
    }
    return 0;
}
static int qflag(const char *q, const char *key) { char v[8]; return qget(q, key, v, sizeof v) && (v[0] == '1' || v[0] == 't' || v[0] == 'y'); }
/* a four-character code from text: as many characters as given, padded with spaces ("APPL", "ttxt") */
static int four(const char *s, uint32_t *out)
{
    uint8_t m[16];
    if (psmac_utf8_macroman(s, m, sizeof m) < 0) return 0;
    unsigned n = slen((const char *)m);
    if (!n || n > 4) return 0;
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) v = v << 8 | (i < n ? m[i] : ' ');
    *out = v;
    return 1;
}
static int take_path(const char *q, const char *key, uint8_t *mr, char *json, unsigned json_max, int required, char *msg)
{
    char u[600];
    if (!qget(q, key, u, sizeof u)) {
        if (required) { sprintf(msg, "%s= is missing (a Mac path, Volume:Folder:File, or :Folder on the startup volume)", key); return 0; }
        u[0] = 0;
    }
    int n = psmac_utf8_macroman(u, mr, 256);
    if (n < 0) { sprintf(msg, "the path has characters that Mac Roman does not have"); return 0; }
    if (slen(u) > 255) { sprintf(msg, "the path is longer than the Mac's 255 characters"); return 0; }
    if (json) js(json, json_max, mr, (unsigned)n);
    return 1;
}

int psmac_files_request(int conn, const char *method, const char *path, const char *q,
                        const uint8_t *body, uint32_t have, int32_t clen, int *code, char *msg, unsigned msg_max)
{
    (void)msg_max;
    int get = eq(method, "GET"), post = eq(method, "POST"), put = eq(method, "PUT");
    int kind = J_NONE;
    if (get && eq(path, "/files/list")) kind = J_LIST;
    else if (get && eq(path, "/files/info")) kind = J_STAT;
    else if (get && eq(path, "/files/data")) kind = J_PULL;
    else if ((put || post) && eq(path, "/files/data")) kind = J_PUSH;
    else if (post && eq(path, "/files/mkdir")) kind = J_MKDIR;
    else if (post && eq(path, "/files/delete")) kind = J_DELETE;
    else if (post && eq(path, "/files/rename")) kind = J_RENAME;
    else if (get && eq(path, "/apps")) kind = J_APPS;
    else if (post && eq(path, "/apps/open")) kind = J_OPEN;
    else if (post && eq(path, "/apps/quit")) kind = J_QUIT;
    if (kind == J_NONE) {
        *code = 404;
        sprintf(msg, "GET /files/list, /files/info, /files/data, /apps; PUT /files/data; POST /files/mkdir, /files/delete, /files/rename, /apps/open, /apps/quit");
        return 0;
    }
    if (!ag) { *code = 503; sprintf(msg, "the agent's block is not set up"); return 0; }
    if (J.kind != J_NONE) { *code = 503; sprintf(msg, "another file job is under way (%s): try again", kind_name[J.kind]); return 0; }
    __builtin_memset(&J, 0, sizeof J);
    J.conn = conn;
    if (kind == J_QUIT) {
        char v[64];
        if (qget(q, "name", v, sizeof v)) {
            if (psmac_utf8_macroman(v, J.path, 32) < 0) { *code = 400; sprintf(msg, "the name has characters that Mac Roman does not have"); return 0; }
            js(J.upath, sizeof J.upath, J.path, slen((const char *)J.path));
        } else if (qget(q, "creator", v, sizeof v)) {
            if (!four(v, &J.fcreator)) { *code = 400; sprintf(msg, "creator= is four characters (\"ttxt\")"); return 0; }
            js4(J.upath, J.fcreator);
        } else { *code = 400; sprintf(msg, "name= (the program's name) or creator= (its signature) is missing"); return 0; }
    } else if (kind != J_APPS && !take_path(q, "path", J.path, J.upath, sizeof J.upath, kind != J_LIST, msg)) { *code = 400; return 0; }
    if (kind == J_RENAME && !take_path(q, "to", J.path2, 0, 0, 1, msg)) { *code = 400; return 0; }
    char v[64];
    if (kind == J_PULL) {
        J.macbinary = qflag(q, "macbinary");
        J.fork = qget(q, "fork", v, sizeof v) && (v[0] == 'r' || v[0] == '1') ? 1 : 0;
    }
    if (kind == J_PUSH) {
        if (clen < 0) { *code = 411; sprintf(msg, "an upload needs Content-Length"); return 0; }
        J.macbinary = qflag(q, "macbinary");
        J.clen = clen;
        J.flags = (qflag(q, "force") ? PSMB_AGF_FORCE : 0) | (qflag(q, "mkdir") ? PSMB_AGF_MKDIR : 0);
        J.ftype = 0x3F3F3F3Fu; J.fcreator = 0x3F3F3F3Fu;         /* '????' unless told */
        if (qget(q, "type", v, sizeof v) && !four(v, &J.ftype)) { *code = 400; sprintf(msg, "type= is four characters (\"TEXT\")"); return 0; }
        if (qget(q, "creator", v, sizeof v) && !four(v, &J.fcreator)) { *code = 400; sprintf(msg, "creator= is four characters (\"ttxt\")"); return 0; }
        if (!J.macbinary && J.path[0] && J.path[slen((const char *)J.path) - 1] == ':') {
            *code = 400; sprintf(msg, "a path ending in ':' is a folder: give the file's name (or send MacBinary, whose name is used)"); return 0;
        }
    }
    if (kind == J_MKDIR) J.flags = qflag(q, "parents") ? PSMB_AGF_MKDIR : 0;
    if (kind == J_LIST) J.index = 1;                            /* LIST counts entries from 1 (the File Manager's index), APPS from 0 */
    J.kind = kind;
    J.t0 = now_ms();
    J.state = S_WAIT_AGENT;
    J.deadline = now_ms() + T_AGENT;
    J.last_rx = now_ms();
    kprintf("[FILES] %s %s\n", kind_name[kind], J.upath);
    if (kind == J_PUSH && have) psmac_files_body(conn, body, have);
    return 1;
}

uint32_t psmac_files_produce(int conn, uint8_t *buf, uint32_t room, int *finished)
{
    *finished = 0;
    if (J.kind == J_NONE || J.conn != conn) { *finished = 1; return 0; }
    if (J.state == S_STREAM && !J.posted) step();          /* room may have come free: the next chunk, the next entries */
    uint32_t n = J.out_len - J.out_pos;
    if (n > room) n = room;
    __builtin_memcpy(buf, jout + J.out_pos, n);
    J.out_pos += n; J.sent += n;
    if (J.out_pos == J.out_len) J.out_pos = J.out_len = 0;
    if (J.done && J.out_pos >= J.out_len) {
        *finished = 1;
        job_end();
    }
    return n;
}

uint32_t psmac_files_room(int conn)
{
    if (J.kind != J_PUSH || J.conn != conn) return 65535;    /* not ours (any more): whatever comes is dropped */
    if (J.discard) return 65535;
    if (J.clen >= 0 && J.body_got >= (uint32_t)J.clen) return 0;
    uint32_t room = JIN_SZ - J.in_len;
    if (J.clen >= 0 && room > (uint32_t)J.clen - J.body_got) room = (uint32_t)J.clen - J.body_got;
    return room;
}
void psmac_files_body(int conn, const uint8_t *data, uint32_t len)
{
    if (J.kind != J_PUSH || J.conn != conn) return;
    if (J.clen >= 0 && len > (uint32_t)J.clen - J.body_got) len = (uint32_t)J.clen - J.body_got;
    J.body_got += len;
    if (len) J.last_rx = now_ms();
    if (J.discard) return;
    if (len > JIN_SZ - J.in_len) len = JIN_SZ - J.in_len;   /* the transport keeps to the room given */
    __builtin_memcpy(jin + J.in_len, data, len);
    J.in_len += len;
}
void psmac_files_gone(int conn)
{
    if (J.kind == J_NONE || J.conn != conn) return;
    J.conn = -1;
    if (J.state == S_DONE) { job_end(); return; }
    if (J.kind == J_PUSH && J.created == 1) {               /* a partial file: deleted once the agent is free */
        J.err_code = 499; sprintf(J.err, "the connection closed during the upload");
        J.created = 2; J.state = S_CLEAN; J.discard = 1;
        return;
    }
    sprintf(J.err, "the connection closed");
    finish_log(0);
    if (!J.posted) job_end();                               /* else the answer under way is awaited first (psmac_files_poll) */
}

void psmac_files_mac_reset(void)
{
    if (!ag) return;
    ag->magic = 0;                                          /* the agent of the next start writes it again */
    mac_was_reset = 1;
}

void psmac_files_init(uint8_t *window)
{
    ag = (volatile struct psmb_agent *)(window + PSMB_OFF_AGENT);
    agbuf = window + PSMB_OFF_AGENTBUF;
    for (unsigned i = 0; i < sizeof *ag; i++) ((volatile uint8_t *)ag)[i] = 0;
    ag->kversion = 1; ag->buf_off = PSMB_OFF_AGENTBUF; ag->buf_size = PSMB_AGENT_BUF;
    mb();
    ag->kmagic = PSMB_AGENT_KMAGIC;
    J.conn = -1;
    const char *o = psmac_option("mac.filestrace");
    trace = o && o[0] == '1';
}

/* GET /files/progress (23 November 2026): the transfer under way, asked on a second connection while the first
   carries the file, so the client shows the bytes the Mac has written or read rather than the bytes its socket
   took. done and total count the forks' bytes (a MacBinary header and padding are not in them); received is an
   upload's body as it came in, sent a download's answer as it went out. Once the job has ended, the last one,
   with active false. */
void psmac_files_progress_json(char *o, unsigned max)
{
    (void)max;
    static const char *const phase[] = { "waiting for the agent", "asking", "sending", "starting", "creating the file", "writing",
                                         "setting its information", "replacing the old file", "replacing the old file", "cleaning up", "done" };
    if (J.kind != J_NONE && J.kind != J_FRONT) {
        uint32_t ms = now_ms() - J.t0;
        int xfer = J.kind == J_PUSH || J.kind == J_PULL;
        const char *ph = J.state >= 0 && J.state <= S_DONE ? phase[J.state] : "";
        if (J.kind == J_PULL && J.state == S_STREAM) ph = "reading";
        sprintf(o, "{\"active\": true, \"job\": \"%s\", \"path\": \"%s\", \"phase\": \"%s\", \"fork\": \"%s\", \"done\": %u, \"total\": %u, "
                   "\"received\": %u, \"length\": %d, \"sent\": %u, \"ms\": %u, \"requests\": %u, \"mac_ms\": %u, \"network_ms\": %u}\n",
                kind_name[J.kind], J.upath, ph, !xfer ? "" : J.fork == 1 ? "rsrc" : J.fork == 0 ? "data" : "", J.bytes, fork_total(),
                J.body_got, J.kind == J_PUSH ? (int)J.clen : -1, J.sent, ms, J.reqs, (uint32_t)((J.queue_us + J.work_us) / 1000u),
                (uint32_t)((J.net_us + us_since(J.net_t)) / 1000u));
        return;
    }
    char l[1000];
    psmac_macroman_utf8((const uint8_t *)last, slen(last), l, sizeof l, 1);
    sprintf(o, "{\"active\": false, \"job\": \"%s\", \"path\": \"%s\", \"done\": %u, \"total\": %u, \"ms\": %u, \"ok\": %s, \"last\": \"%s\"}\n",
            kind_name[lastp.kind], lastp.upath, lastp.done, lastp.total, lastp.ms, lastp.ok ? "true" : "false", l);
}

void psmac_files_state_json(char *o, unsigned max)
{
    (void)max;
    if (!ag) { sprintf(o, "null"); return; }
    char st[40], l[1000];
    unsigned k = cl(ag->startup, 31);
    uint8_t s[32]; cpy(s, ag->startup, k);
    js(st, sizeof st, s, k);
    psmac_macroman_utf8((const uint8_t *)last, slen(last), l, sizeof l, 1);
    uint32_t sv = ag->sysversion, f = ag->features;
    char ver[16];
    if (sv & 15) sprintf(ver, "%u.%u.%u", (sv >> 8) & 15, (sv >> 4) & 15, sv & 15); else sprintf(ver, "%u.%u", (sv >> 8) & 15, (sv >> 4) & 15);
    sprintf(o, "{\"installed\": %s, \"alive\": %s, \"version\": %u, \"system\": \"%s\", \"startup\": \"%s\", \"process_manager\": %s, \"multifinder\": %s, "
               "\"served\": %u, \"chained\": %u, \"repeats\": %u, \"wakes\": %u, \"job\": \"%s\", \"jobs\": %u, \"failed\": %u, \"last\": \"%s\"}",
            agent_installed() ? "true" : "false", agent_alive() ? "true" : "false", agent_installed() ? ag->version : 0,
            agent_installed() ? ver : "",
            agent_installed() ? st : "", agent_installed() && (f & PSMB_AGX_PROCMGR) ? "true" : "false",
            agent_installed() && (f & PSMB_AGX_MULTIFINDER) ? "true" : "false",
            ag->served, ag->chained, ag->repeats, ag->wakes, kind_name[J.kind], jobs, jobs_failed, l);
}

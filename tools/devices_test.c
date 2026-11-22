/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/devices_test.c - Aurora68 Play as a device of the music server (play/core/devices.c), on the host.
 *
 *     devices_test                      the parsers and the state machine, on answers written as the
 *                                       server writes them (tools/music-server, mac_device): make -C mac check
 *     devices_test HOST PORT SECONDS    the same code against a running server, through plain sockets: it
 *                                       announces, reports a state, then polls for commands for SECONDS and
 *                                       prints each command and its tracks (tools/devices_test.sh sends one)
 *
 * Written on 22 November 2026 (docs/08 item 105); since the shared core (item 107) the commands and the device
 * list are read by play/core/proto.c, in place, so the answers here are writable copies.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include "../play/core/include/devices.h"

static int fails, checks;
static void check(int ok, const char *what)
{
	checks++;
	if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static char *w(const char *s, long *n)       /* a writable copy: the core reads answers in place */
{
	static char bufs[4][1024];
	static int k;
	char *b = bufs[k++ & 3];
	*n = (long)strlen(s);
	memcpy(b, s, (size_t)*n + 1);
	return b;
}
static void units(void)
{
	struct dv d;
	char path[400], id[17], id2[17], *b;
	long n;
	struct a68_rec it;
	struct a68_cmd c;
	struct a68_device dev;
	/* ids: "mac-" and twelve letters and digits, different for different seeds, the same for the same */
	dv_make_id(id, 12345, 678);
	dv_make_id(id2, 12346, 678);
	check(strlen(id) == 16 && !strncmp(id, "mac-", 4) && strcmp(id, id2), "an id is mac- and 12 characters, one per seed");
	dv_make_id(id2, 12345, 678);
	check(!strcmp(id, id2), "the same seed, the same id");
	dv_init(&d, "mac-0123456789ab", "Fran\x8Dois's SE", "1.3", "classic");     /* Mac Roman c cedilla */
	check(dv_want(&d, 100) == DV_NONE, "nothing before a server is known");
	dv_start(&d, 100);
	check(dv_want(&d, 100) == DV_ANNOUNCE, "a server known: announce at once");
	dv_path(&d, DV_ANNOUNCE, path, sizeof path);
	check(!strcmp(path, "/mac/announce?id=mac-0123456789ab&name=Fran%8Dois%27s%20SE&kind=classic&v=1.3"), "the announce's path, the name in %XX");
	/* refused, then answered */
	dv_answer(&d, DV_ANNOUNCE, 0, 0, 0, 100);
	check(dv_want(&d, 101) == DV_NONE && dv_want(&d, 105) == DV_ANNOUNCE, "no answer: again five seconds later");
	b = w("ok\r20\r7\r", &n);
	dv_answer(&d, DV_ANNOUNCE, 200, b, n, 105);
	check(d.announced && d.every == 20 && d.gen == 7 && !d.gen_changed, "the announce's answer: every 20 s, generation 7");
	check(dv_want(&d, 105) == DV_STATE, "the state goes with the announce");
	struct dv_state s = { 1, 12, 200, 80, "1a2b3c4d", "", "" };
	dv_set_state(&d, &s, 105);
	dv_path(&d, DV_STATE, path, sizeof path);
	check(!strcmp(path, "/mac/state?id=mac-0123456789ab&p=1&pos=12&len=200&vol=80&t=1a2b3c4d"), "the state's path, a library track");
	b = w("ok\r", &n); dv_answer(&d, DV_STATE, 200, b, n, 105);
	check(dv_want(&d, 105) == DV_COMMANDS, "then the commands");
	dv_path(&d, DV_COMMANDS, path, sizeof path);
	check(!strcmp(path, "/mac/commands?id=mac-0123456789ab&wait=15"), "the commands' path: a wait under MacTCP's timeout");
	b = w("", &n); dv_answer(&d, DV_COMMANDS, 200, b, n, 106);
	check(dv_want(&d, 106) == DV_COMMANDS, "an empty answer (the wait over): ask again at once");
	/* seconds going by are no change; a seek, a pause, another track are */
	s.pos = 20; dv_set_state(&d, &s, 113);
	check(dv_want(&d, 113) == DV_COMMANDS, "eight seconds later, eight seconds on: no report");
	s.pos = 90; dv_set_state(&d, &s, 114);
	check(dv_want(&d, 114) == DV_STATE, "a seek: a report");
	b = w("ok\r", &n); dv_answer(&d, DV_STATE, 200, b, n, 114);
	s.playing = 0; dv_set_state(&d, &s, 117);
	check(dv_want(&d, 117) == DV_STATE, "paused: a report");
	b = w("ok\r", &n); dv_answer(&d, DV_STATE, 200, b, n, 117);
	struct dv_state r = { 1, 0, 0, 80, "", "Groove Salad", "SomaFM" };
	dv_set_state(&d, &r, 120);
	dv_path(&d, DV_STATE, path, sizeof path);
	check(!strcmp(path, "/mac/state?id=mac-0123456789ab&p=1&pos=0&len=0&vol=80&title=Groove%20Salad&artist=SomaFM"), "a station: its title and artist");
	b = w("ok\r", &n); dv_answer(&d, DV_STATE, 200, b, n, 120);
	/* the announce again after "every", and a new generation */
	check(dv_want(&d, 124) == DV_COMMANDS && dv_want(&d, 125) == DV_ANNOUNCE, "the next announce after 20 s");
	b = w("ok\r20\r9\r", &n);
	dv_answer(&d, DV_ANNOUNCE, 200, b, n, 125);
	check(d.gen == 9 && d.gen_changed, "a new generation: new albums, the lists again");
	/* the server restarted: "announce first" */
	b = w("announce first\r", &n); dv_answer(&d, DV_COMMANDS, 404, b, n, 130);
	check(!d.announced && dv_want(&d, 130) == DV_ANNOUNCE, "404 to a poll: announce again");

	/* commands, as mac_command_lines writes them: two, the first with tracks, the second a station */
	b = w("3\tplay\t0\t\r"
	                           "T\t1a2b3c4d\tFirst Song\tAn Artist\t212.5\r"
	                           "T\t5e6f7a8b\tSecond \xD2Song\xD3\tAn Artist\t180\r"
	                           "4\tvolume\t0\t55\r"
	                           "5\tplay\t0\t\r"
	                           "R\tGroove Salad\thttp://ice1.somafm.com/groovesalad-128-mp3\r"
	                           "6\tseek\t95\t\r"
	                           "7\tdance\t0\t\r", &n);              /* a later version's action: told apart, passed on */
	a68_rec_init(&it, b, n);
	check(a68_next_cmd(&it, &c) && c.kind == 'C' && c.n == 3 && a68_action(c.action) == A68_ACT_PLAY && c.volume == -1, "a command: play, no volume");
	check(a68_next_cmd(&it, &c) && c.kind == 'T' && !strcmp(c.tid, "1a2b3c4d") && !strcmp(c.title, "First Song") && c.secs == 212, "its first track");
	check(a68_next_cmd(&it, &c) && c.kind == 'T' && !strcmp(c.title, "Second \xD2Song\xD3"), "its second, Mac Roman quotes kept");
	check(a68_next_cmd(&it, &c) && c.kind == 'C' && a68_action(c.action) == A68_ACT_VOLUME && c.volume == 55, "volume 55");
	check(a68_next_cmd(&it, &c) && c.kind == 'C' && c.n == 5, "a station's play");
	check(a68_next_cmd(&it, &c) && c.kind == 'R' && !strcmp(c.name, "Groove Salad") && !strncmp(c.url, "http://ice1.", 12), "its station");
	check(a68_next_cmd(&it, &c) && c.kind == 'C' && a68_action(c.action) == A68_ACT_SEEK && c.position == 95, "seek to 95 s");
	check(a68_next_cmd(&it, &c) && c.kind == 'C' && a68_action(c.action) == A68_ACT_UNKNOWN, "an action not known");
	check(!a68_next_cmd(&it, &c), "then no more");
	b = w("", &n);
	a68_rec_init(&it, b, n);
	check(!a68_next_cmd(&it, &c), "an empty answer: no commands");
	/* the devices list: the Live channel and two players, one off line, one line with a field missing */
	b = w("live\tLive\tlive\t1\tA Song\tAn Artist\r"
	                           "mac-0123456789ab\tFran\x8Dois's SE\tclassic\t1\t\t\r"
	                           "macos-1\tStudio Mac\tmacos\t0\r", &n);
	a68_rec_init(&it, b, n);
	check(a68_next_device(&it, &dev) && !strcmp(dev.id, "live") && dev.online && !strcmp(dev.title, "A Song"), "Live");
	check(a68_next_device(&it, &dev) && !strcmp(dev.kind, "classic") && !dev.title[0], "this Mac, nothing playing");
	check(a68_next_device(&it, &dev) && !dev.online && !dev.title[0] && !strcmp(dev.name, "Studio Mac"), "off line, fields missing");
	check(!a68_next_device(&it, &dev), "then no more");
}

/* ---- against a server ---------------------------------------------------------------------------------- */
static char body[1 << 20];
static int get(const char *host, const char *port, const char *path, long *len)
{
	struct addrinfo hints = { 0 }, *ai;
	int fd, status = 0;
	long n = 0, k;
	char req[1024];
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &ai)) return 0;
	fd = socket(ai->ai_family, ai->ai_socktype, 0);
	if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen)) { freeaddrinfo(ai); return 0; }
	freeaddrinfo(ai);
	snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s:%s\r\nUser-Agent: Aurora68-Play/1.3 (Macintosh)\r\nX-Aurora: 1\r\n\r\n", path, host, port);
	if (write(fd, req, strlen(req)) < 0) { close(fd); return 0; }
	while ((k = read(fd, body + n, sizeof body - 1 - (size_t)n)) > 0) n += k;
	close(fd);
	body[n] = 0;
	if (sscanf(body, "HTTP/%*s %d", &status) != 1) return 0;
	char *e = strstr(body, "\r\n\r\n");
	if (!e) return 0;
	*len = n - (e + 4 - body);
	memmove(body, e + 4, (size_t)*len);
	body[*len] = 0;
	return status;
}
static int live(const char *host, const char *port, int secs)
{
	struct dv d;
	char path[400];
	time_t t0 = time(0);
	int got_cmd = 0;
	dv_init(&d, "mac-hosttest0001", "Host test", "1.3", "classic");
	dv_start(&d, 0);
	while (time(0) - t0 < secs) {
		unsigned long now = (unsigned long)(time(0) - t0);
		int w = dv_want(&d, now);
		if (w == DV_NONE) { sleep(1); continue; }
		dv_path(&d, w, path, sizeof path);
		long len = 0;
		int st = get(host, port, path, &len);
		printf("%s -> %d, %ld bytes\n", path, st, len);
		fflush(stdout);
		dv_answer(&d, w, st, body, len, (unsigned long)(time(0) - t0));
		if (w == DV_ANNOUNCE && d.announced) {
			printf("announced: every %lu s, generation %lu\n", d.every, d.gen);
			struct dv_state s = { 1, 3, 120, 70, "", "Test station", "Aurora68 test" };
			dv_set_state(&d, &s, now);
			long l2 = 0;
			if (get(host, port, "/mac/devices", &l2) == 200) {
				struct a68_rec it;
				struct a68_device dev;
				a68_rec_init(&it, body, l2);
				while (a68_next_device(&it, &dev)) printf("device %s \"%s\" %s %s\n", dev.id, dev.name, dev.kind, dev.online ? "online" : "offline");
			}
		}
		if (w == DV_COMMANDS && st == 200 && len) {
			struct a68_rec it;
			struct a68_cmd c;
			a68_rec_init(&it, body, len);
			while (a68_next_cmd(&it, &c)) {
				if (c.kind == 'C') { printf("command %ld %s position %ld volume %d\n", c.n, c.action, c.position, c.volume); got_cmd++; }
				else if (c.kind == 'T') printf("  track %s \"%s\" by \"%s\", %lu s\n", c.tid, c.title, c.artist, c.secs);
				else printf("  station \"%s\" %s\n", c.name, c.url);
			}
			fflush(stdout);
		}
	}
	return got_cmd;
}

int main(int argc, char **argv)
{
	if (argc >= 4) return live(argv[1], argv[2], atoi(argv[3])) ? 0 : 1;
	units();
	printf("devices_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}

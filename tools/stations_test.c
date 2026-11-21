/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/stations_test.c - Aurora68 Play's station list (mac/piplay/stations.c) on the host: `make -C mac
 * check`. The built-in list, a flat file of the first kind (before 21 November 2026), headings and their
 * odd cases, and Radio > Add Default Stations: what it adds to a flat file, to its own result (nothing)
 * and to a file the user changed. Written on 21 November 2026 (docs/08 item 87).
 */
#include <stdio.h>
#include <string.h>
#include "../mac/piplay/stations.h"

static int fails, checks;
static void check(int ok, const char *what)
{
	checks++;
	if (!ok) { fails++; printf("FAIL %s\n", what); }
}
static struct stations l, l2;

/* a menu's items as "Name|Group>|..." for comparing */
static const char *menu(const struct stations *s, int group)
{
	static char b[4096];
	short it[MAX_ITEMS];
	int n = stations_items(s, group, it, MAX_ITEMS), k;
	b[0] = 0;
	for (k = 0; k < n; k++) {
		if (k) strcat(b, "|");
		if (it[k] >= 0) strcat(b, s->station[it[k]].name);
		else { strcat(b, s->group[-1 - it[k]].name); strcat(b, ">"); }
	}
	return b;
}
static int group_named(const struct stations *s, const char *name, int parent)
{
	int g;
	for (g = 0; g < s->ngroups; g++)
		if (s->group[g].parent == parent && !strcmp(s->group[g].name, name)) return g;
	return -99;
}

/* the built-in list of the first kind: twelve SomaFM channels and NPO Radio 2, no headings */
static const char old_builtin[] =
	"# Aurora68 Play Stations: the internet radio stations in Aurora68 Play's Radio menu.\r"
	"# SomaFM: listener-supported, commercial-free radio from San Francisco (somafm.com).\r"
	"Groove Salad = http://ice2.somafm.com/groovesalad-128-mp3\r"
	"Drone Zone = http://ice2.somafm.com/dronezone-128-mp3\r"
	"Secret Agent = http://ice2.somafm.com/secretagent-128-mp3\r"
	"Lush = http://ice2.somafm.com/lush-128-mp3\r"
	"Space Station Soma = http://ice2.somafm.com/spacestation-128-mp3\r"
	"Indie Pop Rocks! = http://ice2.somafm.com/indiepop-128-mp3\r"
	"Beat Blender = http://ice2.somafm.com/beatblender-128-mp3\r"
	"Deep Space One = http://ice2.somafm.com/deepspaceone-128-mp3\r"
	"DEF CON Radio = http://ice2.somafm.com/defcon-128-mp3\r"
	"Left Coast 70s = http://ice2.somafm.com/seventies-128-mp3\r"
	"Underground 80s = http://ice2.somafm.com/u80s-128-mp3\r"
	"Sonic Universe = http://ice2.somafm.com/sonicuniverse-128-mp3\r"
	"#\r"
	"NPO Radio 2 = http://icecast.omroep.nl/radio2-bb-mp3\r";

int main(void)
{
	char text[65536], out[16384];
	int added, g, k;
	long n;

	/* the built-in list */
	stations_parse(&l, stations_builtin, stations_builtin_len);
	check(l.nstations == 40, "built-in: 40 stations");
	check(l.ngroups == 12, "built-in: 12 groups (3 and 9 countries)");
	check(!strcmp(menu(&l, ST_TOP), "SomaFM>|Listener-supported>|By country>"), "built-in: the Radio menu's own items");
	g = group_named(&l, "SomaFM", ST_TOP);
	check(g >= 0 && !strncmp(menu(&l, g), "Groove Salad|Drone Zone|", 24) && l.station[11].group == g, "built-in: SomaFM's twelve");
	g = group_named(&l, "By country", ST_TOP);
	check(!strcmp(menu(&l, g), "Netherlands>|Switzerland>|Austria>|United Kingdom>|France>|Germany>|Greece>|New Zealand>|United States>"),
	      "built-in: By country's submenus");
	int nl = group_named(&l, "Netherlands", g), at = group_named(&l, "Austria", g), us = group_named(&l, "United States", g);
	check(!strcmp(menu(&l, nl), "NPO Radio 2|NPO 3FM|NPO Klassiek"), "built-in: the Netherlands");
	check(!strcmp(menu(&l, at), "\x85" "1"), "built-in: Austria's station in Mac Roman");
	check(!strcmp(menu(&l, us), "KEXP Seattle|WFMU Freeform|WWOZ New Orleans|181.fm The Beatles|181.fm Classic Hits"), "built-in: the United States");
	check(!strcmp(menu(&l, group_named(&l, "Listener-supported", ST_TOP)),
	              "Radio Paradise Main|Radio Paradise Mellow|Radio Paradise Rock|Radio Paradise Global|KEXP Seattle|WFMU Freeform|"
	              "WWOZ New Orleans|Radio Swiss Jazz|Radio Swiss Classic|Radio Swiss Pop"), "built-in: listener-supported");
	for (k = 0; k < l.nstations; k++) {
		check(!strncmp(l.station[k].url, "http://", 7), "built-in: every address plain http");
		check(strlen(l.station[k].name) < 31, "built-in: every name short enough for the menu");
	}
	for (k = 0; k < (int)stations_builtin_len; k++)
		check(stations_builtin[k] != '\n' && (unsigned char)stations_builtin[k] != 0xD6, "built-in: Return-ended lines, no Latin-1");
	stations_missing(&l, stations_builtin, stations_builtin_len, out, sizeof out, &added);
	check(added == 0, "built-in: Add Default Stations on the built-in list adds nothing");

	/* a file of the first kind: the flat menu as before */
	stations_parse(&l, old_builtin, sizeof old_builtin - 1);
	check(l.nstations == 13 && l.ngroups == 0, "flat: 13 stations, no groups");
	check(!strncmp(menu(&l, ST_TOP), "Groove Salad|Drone Zone|", 24) && strstr(menu(&l, ST_TOP), "|Sonic Universe|NPO Radio 2"), "flat: the menu as before");

	/* Add Default Stations on it: the groups it lacks; SomaFM's channels and NPO Radio 2, there at the top, not again */
	n = stations_missing(&l, stations_builtin, stations_builtin_len, out, sizeof out, &added);
	check(added == 27, "flat: Add Default Stations adds 27");
	out[n] = 0;
	check(!strstr(out, "[SomaFM]") && !strstr(out, "Groove Salad") && !strstr(out, "NPO Radio 2"), "flat: nothing it has already");
	check(strstr(out, "\r[Listener-supported]\r# Radio Paradise") != 0, "flat: a heading with its notes");
	check(strstr(out, "[By country/Netherlands]\r# NPO, Dutch public radio") && strstr(out, "NPO 3FM = "), "flat: the Netherlands without Radio 2");
	memcpy(text, old_builtin, sizeof old_builtin - 1);
	memcpy(text + sizeof old_builtin - 1, out, n);
	n += sizeof old_builtin - 1;
	stations_parse(&l2, text, n);
	check(l2.nstations == 40, "flat + defaults: 40 stations");
	const char *top = menu(&l2, ST_TOP), *tail = "|NPO Radio 2|Listener-supported>|By country>";
	check(strlen(top) > strlen(tail) && !strcmp(top + strlen(top) - strlen(tail), tail),
	      "flat + defaults: the old stations, then the new groups");
	stations_missing(&l2, stations_builtin, stations_builtin_len, out, sizeof out, &added);
	check(added == 0, "flat + defaults: a second Add Default Stations adds nothing");

	/* a file the user changed: a group renamed, a station taken out, one of their own */
	static const char user[] =
		"My Station = http://radio.testbed:8000/stream\n"
		"[somafm]\n"                                    /* another case: the same group */
		"Groove Salad = http://ice2.somafm.com/groovesalad-128-mp3\n"
		"[By country / Netherlands]\n"                  /* spaces round the slash */
		"NPO Radio 2 = http://icecast.omroep.nl/radio2-bb-mp3\n"
		"NPO 3FM = http://icecast.omroep.nl/3fm-bb-mp3\n"
		"NPO Klassiek = http://icecast.omroep.nl/radio4-bb-mp3\n";
	stations_parse(&l, user, sizeof user - 1);
	check(l.nstations == 5 && l.ngroups == 3, "user: 5 stations, 3 groups");
	n = stations_missing(&l, stations_builtin, stations_builtin_len, out, sizeof out, &added);
	out[n] = 0;
	check(added == 40 - 4, "user: the 36 it lacks");
	check(strstr(out, "\r[SomaFM]\r# SomaFM") && strstr(out, "Drone Zone = ") && !strstr(out, "Groove Salad"), "user: SomaFM's eleven others");
	check(!strstr(out, "Netherlands"), "user: the Netherlands complete, not written");
	memcpy(text, user, sizeof user - 1);
	memcpy(text + sizeof user - 1, out, n);
	stations_parse(&l2, text, n + sizeof user - 1);
	g = group_named(&l2, "somafm", ST_TOP);
	check(g >= 0 && group_named(&l2, "SomaFM", ST_TOP) == -99 && !strncmp(menu(&l2, g), "Groove Salad|Drone Zone|", 24),
	      "user: [SomaFM] added to their [somafm], in order");
	check(!strcmp(menu(&l2, ST_TOP), "My Station|somafm>|By country>|Listener-supported>"), "user: the Radio menu's items");

	/* headings' odd cases */
	static const char odd[] =
		"Top One = http://a/1\r"
		"[Empty]\r"                                     /* no station: no submenu */
		"# just a note\r"
		"[A/B/C]\r"                                     /* one level: \"B/C\" is the submenu's name */
		"http://host.example:8000/x\r"                  /* a bare address: named after its host */
		"[]\r"                                          /* back to the top */
		"Top Two = http://a/2\r"
		"[A]\r"
		"In A = http://a/3\r"
		"[Weird] = http://a/4\r"                        /* a station whose name has brackets */
		"http://[fd68::80]:8000/stream\r"              /* an IPv6 address: named with its brackets */
		"[/Sub]\r"                                      /* no group name: the top */
		"Top Three = http://a/5\r";
	stations_parse(&l, odd, sizeof odd - 1);
	check(!strcmp(menu(&l, ST_TOP), "Top One|A>|Top Two|Top Three"), "odd: the top, the empty group left out");
	g = group_named(&l, "A", ST_TOP);
	check(!strcmp(menu(&l, g), "B/C>|In A|[Weird]|[fd68::80]"), "odd: A's items");
	check(!strcmp(menu(&l, group_named(&l, "B/C", g)), "host.example"), "odd: B/C's bare address");

	/* limits: more stations and groups than there is room for */
	char *p = text;
	for (k = 0; k < 200; k++) p += sprintf(p, "[G%d]\rS%d = http://h/%d\r", k, k, k);
	stations_parse(&l, text, p - text);
	check(l.nstations == MAX_STATIONS && l.ngroups == MAX_GROUPS && l.norder <= MAX_ITEMS, "limits: kept to the arrays");
	check(l.station[MAX_GROUPS].group == ST_TOP, "limits: past the last group, stations at the top");
	n = stations_missing(&l, stations_builtin, stations_builtin_len, out, 200, &added);
	check(n <= 200 && added >= 1 && out[n - 1] == '\r', "limits: Add Default Stations cut at whole lines");

	printf("stations_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}

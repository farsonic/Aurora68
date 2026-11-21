/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/stations.c - Aurora68 Play's station list: the built-in list, the file taken apart into
 * stations and groups (the Radio menu's submenus), and the built-in stations a user's file lacks.
 *
 * Backstory (21 November 2026, docs/08 item 87). The Radio menu began (item 83) as one flat list of
 * SomaFM's twelve channels, one more (NPO Radio 2) joined it for IPv6, and the owner asked for many more
 * stations, grouped. One flat menu of forty stations would not fit the SE's 342 lines, so the file got
 * headings, "[Group]" and "[Group/Sub]", and the menu hierarchical submenus (piplay.c). A file without
 * headings, as every user has today, still makes the flat menu. The file is the user's: Aurora68 Play
 * writes it only when it is not there, and Radio > Add Default Stations only adds at its end.
 *
 * The built-in list's addresses all answered on 21 November 2026 with 200 or 206 and audio/mpeg over
 * plain http (from a test machine; the Radio Swiss ones after two redirections, which radio.c follows).
 * Stations that answered only over https, or not at all, were left out (ABC, FIP, Deutschlandfunk, RAI,
 * Jazz24, Radio Caroline, Linn). Names are in Mac Roman: Ö is $85.
 *
 * No Toolbox calls and no allocation: tools/stations_test.c runs this file on the host.
 */
#include "stations.h"

const char stations_builtin[] =
	"# Aurora68 Play Stations: the internet radio stations in Aurora68 Play's Radio menu.\r"
	"# One station a line: its name, an equals sign, its address:\r"
	"#     Name = http://host[:port]/path\r"
	"# A line [Group] starts a submenu of the Radio menu, a line [Group/Sub] a submenu inside\r"
	"# Group's; the stations under it, up to the next such line, are its items. Stations above\r"
	"# the first of these lines, or below a line [], stand in the Radio menu itself.\r"
	"# Plain http:// MP3 streams (Shoutcast, Icecast) only; lines that start with # are notes.\r"
	"# Change the list with TeachText; Aurora68 Play reads it again when it comes to the front.\r"
	"# Throw this file away and Aurora68 Play writes the list below again; Radio > Add Default\r"
	"# Stations adds those of the list below that this file lacks, at its end.\r"
	"#\r"
	"[SomaFM]\r"
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
	"[Listener-supported]\r"
	"# Radio Paradise (California), KEXP (Seattle), WFMU (New Jersey) and WWOZ (New Orleans) are paid\r"
	"# for by their listeners; Radio Swiss is the Swiss public broadcaster's music without talk.\r"
	"Radio Paradise Main = http://stream.radioparadise.com/mp3-192\r"
	"Radio Paradise Mellow = http://stream.radioparadise.com/mellow-192\r"
	"Radio Paradise Rock = http://stream.radioparadise.com/rock-192\r"
	"Radio Paradise Global = http://stream.radioparadise.com/global-192\r"
	"KEXP Seattle = http://kexp-mp3-128.streamguys1.com/kexp128.mp3\r"
	"WFMU Freeform = http://stream0.wfmu.org/freeform-128k.mp3\r"
	"WWOZ New Orleans = http://wwoz-sc.streamguys1.com/wwoz-hi.mp3\r"
	"Radio Swiss Jazz = http://stream.srg-ssr.ch/m/rsj/mp3_128\r"
	"Radio Swiss Classic = http://stream.srg-ssr.ch/m/rsc_de/mp3_128\r"
	"Radio Swiss Pop = http://stream.srg-ssr.ch/m/rsp/mp3_128\r"
	"#\r"
	"[By country/Netherlands]\r"
	"# NPO, Dutch public radio. Its servers have IPv6 as well as IPv4: with Riptide's\r"
	"# \"mactcp ipv6 prefer\" the window says IPv6 after the counts, else IPv4.\r"
	"NPO Radio 2 = http://icecast.omroep.nl/radio2-bb-mp3\r"
	"NPO 3FM = http://icecast.omroep.nl/3fm-bb-mp3\r"
	"NPO Klassiek = http://icecast.omroep.nl/radio4-bb-mp3\r"
	"[By country/Switzerland]\r"
	"Radio Swiss Jazz = http://stream.srg-ssr.ch/m/rsj/mp3_128\r"
	"Radio Swiss Classic = http://stream.srg-ssr.ch/m/rsc_de/mp3_128\r"
	"Radio Swiss Pop = http://stream.srg-ssr.ch/m/rsp/mp3_128\r"
	"[By country/Austria]\r"
	"\x85" "1 = http://ors-sn05.ors-shoutcast.at/oe1-q2a\r"
	"[By country/United Kingdom]\r"
	"Classic FM = http://media-ice.musicradio.com/ClassicFMMP3\r"
	"BBC World Service = http://stream.live.vc.bbcmedia.co.uk/bbc_world_service\r"
	"[By country/France]\r"
	"Radio Nova = http://novazz.ice.infomaniak.ch/novazz-128.mp3\r"
	"[By country/Germany]\r"
	"90s90s Pop = http://streams.90s90s.de/pop/mp3-128/\r"
	"[By country/Greece]\r"
	"ERT Kosmos = http://radiostreaming.ert.gr/ert-kosmos\r"
	"[By country/New Zealand]\r"
	"RNZ Concert = http://radionz-ice.streamguys.com/concert.mp3\r"
	"[By country/United States]\r"
	"KEXP Seattle = http://kexp-mp3-128.streamguys1.com/kexp128.mp3\r"
	"WFMU Freeform = http://stream0.wfmu.org/freeform-128k.mp3\r"
	"WWOZ New Orleans = http://wwoz-sc.streamguys1.com/wwoz-hi.mp3\r"
	"181.fm The Beatles = http://listen.181fm.com/181-beatles_128k.mp3\r"
	"181.fm Classic Hits = http://listen.181fm.com/181-greatoldies_128k.mp3\r";
const long stations_builtin_len = sizeof stations_builtin - 1;

#define NO_GROUP (-2)                    /* a heading the list does not have */

/* the next line of t from *i: [*a, *e) without the spaces around it; 0 at the end of the text */
static int next_line(const char *t, long n, long *i, long *a, long *e)
{
	long k = *i;
	if (k >= n) return 0;
	*a = k;
	while (k < n && t[k] != '\r' && t[k] != '\n') k++;
	*e = k;
	if (k < n && t[k] == '\r') k++;          /* one line end: Return, line feed, or both */
	if (k < n && t[k] == '\n') k++;
	*i = k;
	while (*a < *e && (t[*a] == ' ' || t[*a] == '\t')) (*a)++;
	while (*e > *a && (t[*e - 1] == ' ' || t[*e - 1] == '\t')) (*e)--;
	return 1;
}
static int is_note(const char *t, long a, long e) { return a == e || t[a] == '#' || t[a] == ';'; }
static int is_heading(const char *t, long a, long e) { return e - a >= 2 && t[a] == '[' && t[e - 1] == ']'; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* a stored name against [a, e) of t, letters of either case alike */
static int same_name(const char *s, const char *t, long a, long e)
{
	while (a < e && *s && lower((unsigned char)*s) == lower((unsigned char)t[a])) { s++; a++; }
	return a == e && !*s;
}
static int same_text(const char *s, const char *t, long a, long e)
{
	while (a < e && *s == t[a]) { s++; a++; }
	return a == e && !*s;
}
static void copy_span(char *d, int max, const char *t, long a, long e)
{
	int o = 0;
	while (a < e && o < max - 1) d[o++] = t[a++];
	d[o] = 0;
}
static void trim(const char *t, long *a, long *e)
{
	while (*a < *e && (t[*a] == ' ' || t[*a] == '\t')) (*a)++;
	while (*e > *a && (t[*e - 1] == ' ' || t[*e - 1] == '\t')) (*e)--;
}

/* a heading's inside [a, e): the group's name [*ga, *ge) and the submenu's [*sa, *se) (empty if none) */
static void split_heading(const char *t, long a, long e, long *ga, long *ge, long *sa, long *se)
{
	long k = a;
	while (k < e && t[k] != '/') k++;
	*ga = a; *ge = k;
	*sa = k < e ? k + 1 : e; *se = e;
	trim(t, ga, ge);
	trim(t, sa, se);
}
static int find_group(const struct stations *l, const char *t, long a, long e, int parent)
{
	int g;
	for (g = 0; g < l->ngroups; g++)
		if (l->group[g].parent == parent && same_name(l->group[g].name, t, a, e)) return g;
	return NO_GROUP;
}
static int add_group(struct stations *l, const char *t, long a, long e, int parent)
{
	int g = find_group(l, t, a, e, parent);
	if (g != NO_GROUP) return g;
	if (l->ngroups >= MAX_GROUPS) return parent;         /* too many: its stations go to the one above */
	g = l->ngroups++;
	copy_span(l->group[g].name, sizeof l->group[g].name, t, a, e);
	l->group[g].parent = (short)parent;
	l->group[g].used = 0;
	l->order[l->norder++] = (short)(-1 - g);
	return g;
}
/* the group a heading line [a, e) names: ST_TOP for "[]", NO_GROUP if the list has no such group */
static int find_heading(const struct stations *l, const char *t, long a, long e)
{
	long ga, ge, sa, se;
	int g;
	split_heading(t, a + 1, e - 1, &ga, &ge, &sa, &se);
	if (ga == ge) return ST_TOP;
	g = find_group(l, t, ga, ge, ST_TOP);
	return g < 0 || sa == se ? g : find_group(l, t, sa, se, g);
}
/* the same, the group (and the one above it) made if the list does not have it yet */
static int make_heading(struct stations *l, const char *t, long a, long e)
{
	long ga, ge, sa, se;
	int g;
	split_heading(t, a + 1, e - 1, &ga, &ge, &sa, &se);
	if (ga == ge) return ST_TOP;
	g = add_group(l, t, ga, ge, ST_TOP);
	return g < 0 || sa == se ? g : add_group(l, t, sa, se, g);
}
/* a station's line [a, e): its address [*ua, *ue) and its name [*na, *ne) (empty: a bare address) */
static void split_station(const char *t, long a, long e, long *na, long *ne, long *ua, long *ue)
{
	long eq = a;
	while (eq < e && t[eq] != '=') eq++;
	if (eq < e) { *na = a; *ne = eq; *ua = eq + 1; *ue = e; }
	else { *na = *ne = a; *ua = a; *ue = e; }
	trim(t, na, ne);
	trim(t, ua, ue);
}

void stations_parse(struct stations *l, const char *t, long n)
{
	long i = 0, a, e;
	int cur = ST_TOP;
	l->nstations = l->ngroups = l->norder = 0;
	while (next_line(t, n, &i, &a, &e)) {
		if (is_note(t, a, e)) continue;
		if (is_heading(t, a, e)) { cur = make_heading(l, t, a, e); continue; }
		if (l->nstations >= MAX_STATIONS) continue;
		long na, ne, ua, ue;
		split_station(t, a, e, &na, &ne, &ua, &ue);
		if (ua == ue) continue;
		struct station *s = &l->station[l->nstations];
		copy_span(s->url, sizeof s->url, t, ua, ue);
		if (na == ne) {                          /* a bare address: its host is its name */
			const char *h = s->url;
			int o = 0;
			if (lower(h[0]) == 'h' && lower(h[1]) == 't' && lower(h[2]) == 't' && lower(h[3]) == 'p' && h[4] == ':' && h[5] == '/' && h[6] == '/') h += 7;
			if (*h == '[')                       /* an IPv6 address: up to its closing bracket */
				while (*h && o < (int)sizeof s->name - 1 && (s->name[o++] = *h++) != ']') { }
			else
				while (*h && *h != '/' && *h != ':' && o < (int)sizeof s->name - 1) s->name[o++] = *h++;
			s->name[o] = 0;
		} else copy_span(s->name, sizeof s->name, t, na, ne);
		s->group = (short)cur;
		if (cur >= 0) {                          /* the group and the one above it have a station now */
			l->group[cur].used = 1;
			if (l->group[cur].parent >= 0) l->group[l->group[cur].parent].used = 1;
		}
		l->order[l->norder++] = (short)l->nstations++;
	}
}

int stations_items(const struct stations *l, int group, short *items, int max)
{
	int k, c = 0;
	for (k = 0; k < l->norder && c < max; k++) {
		int v = l->order[k];
		if (v >= 0 ? l->station[v].group == group : l->group[-1 - v].parent == group && l->group[-1 - v].used)
			items[c++] = (short)v;
	}
	return c;
}

static int have_station(const struct stations *l, const char *t, long ua, long ue, int group)
{
	int k;
	for (k = 0; k < l->nstations; k++)
		if ((l->station[k].group == group || l->station[k].group == ST_TOP) && same_text(l->station[k].url, t, ua, ue))
			return 1;
	return 0;
}
static int put(char *out, long *o, long max, const char *s, long n)
{
	long k;
	if (*o + n > max) return 0;
	for (k = 0; k < n; k++) out[*o + k] = s[k];
	*o += n;
	return 1;
}
long stations_missing(const struct stations *l, const char *d, long n, char *out, long max, int *added)
{
	long i = 0, a, e, o = 0;
	const char *head = "[]";                     /* the heading the stations now come under, and its length */
	long head_n = 2;
	long note_a = 0, note_e = 0;                 /* the notes right under it: written with it */
	int group = ST_TOP, written = 0, in_notes = 0;
	*added = 0;
	while (next_line(d, n, &i, &a, &e)) {
		if (is_note(d, a, e)) {
			if (in_notes) note_e = i;
			continue;
		}
		if (is_heading(d, a, e)) {
			head = d + a; head_n = e - a;
			group = find_heading(l, d, a, e);   /* NO_GROUP: only a station at the top counts as there */
			written = 0;
			note_a = note_e = i; in_notes = 1;
			continue;
		}
		in_notes = 0;
		long na, ne, ua, ue;
		split_station(d, a, e, &na, &ne, &ua, &ue);
		if (ua == ue || have_station(l, d, ua, ue, group)) continue;
		long keep = o;
		if (!written) {
			if (!put(out, &o, max, "\r", 1) || !put(out, &o, max, head, head_n) || !put(out, &o, max, "\r", 1) ||
			    !put(out, &o, max, d + note_a, note_e - note_a)) { o = keep; break; }
		}
		if (!put(out, &o, max, d + a, e - a) || !put(out, &o, max, "\r", 1)) { o = keep; break; }
		written = 1;
		(*added)++;
	}
	return o;
}

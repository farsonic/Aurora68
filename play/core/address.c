/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/address.c - addresses, URLs, the order of a name's addresses and the probe's answer
 * (address.h says what and why).
 *
 * Backstory (22 November 2026, docs/08 item 107). Three copies of this lived in the two Plays: the classic
 * Play's radio.c read a station's address (with an IPv6 address in brackets since 21 November, item 88),
 * its library.c read an address typed into Connect to Server... and its fetch.c the probe's answer; the
 * macOS Play's Address.swift did the same in Swift. They differed in small ways that are now one way:
 *   - a typed address: spaces around it ignored, "https://" left out as "http://" is (the macOS rule), a
 *     bracket that is not closed refused; the port when none is typed is the server's own, 8000 (the classic
 *     Play said 8423, the port of the owner's server, which is what the examples show; both now say 8000);
 *   - the probe's answer: its version is checked (the classic rule), its addresses too (the macOS rule);
 *   - the hosts to try after a probe: the preferred family's first, and for IPv4 the address the answer came
 *     from before the one the server names (the classic rule: the server inside a container may name the
 *     container's address); the macOS Play used to try the named one first.
 */
#include "address.h"
#include "text.h"

static int hex(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int a68_ipv4_value(const char *h, unsigned long *a)
{
	unsigned long v = 0, part;
	int n, digits;
	for (n = 0; n < 4; n++) {
		if (*h < '0' || *h > '9') return 0;
		for (part = 0, digits = 0; *h >= '0' && *h <= '9'; h++, digits++) part = part * 10 + (unsigned long)(*h - '0');
		if (part > 255 || digits > 3) return 0;
		v = v << 8 | part;
		if (n < 3 && *h++ != '.') return 0;
	}
	if (*h) return 0;
	*a = v & 0xFFFFFFFFul;
	return 1;
}
int a68_is_ipv4(const char *h) { unsigned long a; return h && a68_ipv4_value(h, &a); }
int a68_is_ipv6(const char *h)
{
	int colon = 0;
	if (!h) return 0;
	if (*h == '[') h++;
	for (; *h && *h != '%' && *h != ']'; h++) {      /* a zone ("%en0") may be any text */
		if (*h == ':') colon = 1;
		else if (!hex((unsigned char)*h) && *h != '.') return 0;
	}
	return colon;
}
int a68_family(const char *h) { return a68_is_ipv4(h) ? A68_IPV4 : a68_is_ipv6(h) ? A68_IPV6 : A68_NAME; }
char *a68_put_ipv4(char *p, char *end, unsigned long a)
{
	int i;
	for (i = 24; i >= 0; i -= 8) {
		p = a68_put_num(p, end, a >> i & 255);
		if (i && p < end) *p++ = '.';
	}
	return p;
}

char *a68_put_url_host(char *p, char *end, const char *h)
{
	int v6 = a68_is_ipv6(h);
	if (*h == '[') h++;
	if (v6 && p < end) *p++ = '[';
	for (; *h && *h != ']'; h++) {
		if (v6 && *h == '%') { p = a68_put(p, end, "%25"); continue; }
		if (p < end) *p++ = *h;
	}
	if (v6 && p < end) *p++ = ']';
	return p;
}
long a68_url(char *out, long max, const char *host, unsigned port, const char *path)
{
	char *p = out, *end = out + max - 1;
	if (max < 8) return 0;
	p = a68_put(p, end, "http://");
	p = a68_put_url_host(p, end, host);
	if (p < end) *p++ = ':';
	p = a68_put_num(p, end, port);
	p = a68_put(p, end, path);
	if (p >= end) { out[0] = 0; return 0; }
	*p = 0;
	return (long)(p - out);
}

int a68_parse_address(const char *s, unsigned def, char *host, long hmax, unsigned *port)
{
	long a, e, k = 0, colons = 0, i;
	unsigned long p = def;
	host[0] = 0;
	while (a68_space(*s) || *s == '\r' || *s == '\n') s++;
	e = a68_len(s);
	while (e > 0 && (a68_space(s[e - 1]) || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
	a = 0;
	if (a68_prefix_ci(s, "http://")) a = 7;
	else if (a68_prefix_ci(s, "https://")) a = 8;
	if (s[a] == '[') {                               /* [v6] or [v6]:port */
		for (i = a + 1; i < e && s[i] != ']'; i++) { }
		if (i >= e) return 0;
		a68_copy_n(host, s + a + 1, i - a - 1, hmax);
		k = i - a - 1;
		a = i + 1;
	} else {
		int slash = 0;
		for (i = a; i < e; i++) { colons += s[i] == ':'; slash |= s[i] == '/'; }
		if (colons > 1 && !slash) {                  /* a bare IPv6 address: no port can follow */
			a68_copy_n(host, s + a, e - a, hmax);
			k = e - a;
			a = e;
		} else {
			for (i = a; i < e && s[i] != ':' && s[i] != '/'; i++) { }
			a68_copy_n(host, s + a, i - a, hmax);
			k = i - a;
			a = i;
		}
	}
	if (a < e && s[a] == ':') {
		int digits = 0;
		for (p = 0, a++; a < e && s[a] >= '0' && s[a] <= '9' && digits < 6; a++, digits++) p = p * 10 + (unsigned long)(s[a] - '0');
		if (!digits || p < 1 || p > 65535) return 0;
	}
	if (!k || k >= hmax) return 0;
	for (i = 0; host[i]; i++)
		if (host[i] == ' ' || host[i] == '\t') return 0;
	*port = (unsigned)p;
	return 1;
}

const char *a68_parse_url(const char *u, char *host, long hmax, unsigned *port, char *path, long pmax, int *bracketed)
{
	long i = 0;
	unsigned long p = 80;
	host[0] = 0;
	path[0] = 0;
	*bracketed = 0;
	while (*u == ' ') u++;
	if (a68_prefix_ci(u, "https://")) return "Aurora68 Play plays plain http:// streams only: a Macintosh cannot do the encryption of https.";
	if (a68_prefix_ci(u, "http://")) u += 7;
	else if (a68_prefix_ci(u, "icy://")) u += 6;
	else {
		const char *c = u;
		while (*c && *c != '/' && *c != ':') c++;
		if (c[0] == ':' && c[1] == '/') return "Aurora68 Play plays http:// addresses only.";
	}
	if (*u == '[') {                                 /* an IPv6 address (RFC 3986's IP-literal) */
		for (u++; *u && *u != ']' && i < hmax - 1; ) host[i++] = *u++;
		host[i] = 0;
		if (*u++ != ']') return "That address has an IPv6 address without its closing bracket.";
		*bracketed = 1;
	} else {
		while (*u && *u != '/' && *u != ':' && i < hmax - 1) host[i++] = *u++;
		host[i] = 0;
	}
	if (!i || (*u && *u != '/' && *u != ':')) return "That address has no station's name in it.";
	if (*u == ':') {
		u++;
		if (*u < '0' || *u > '9') return "The port number in that address is not a number.";
		p = 0;
		while (*u >= '0' && *u <= '9' && p < 100000ul) p = p * 10 + (unsigned long)(*u++ - '0');
		if (!p || p > 65535) return "The port number in that address is not a port.";
	}
	*port = (unsigned)p;
	a68_copy(path, *u == '/' ? u : "/", pmax);
	for (i = 0; path[i]; i++)
		if (path[i] == ' ') return "There is a space in that address.";
	return 0;
}

int a68_order(const char *const *h, int n, int prefer, int *order)
{
	int pass, i, j, c = 0;
	int other = prefer == A68_IPV6 ? A68_IPV4 : A68_IPV6;
	for (pass = 0; pass < 3; pass++)
		for (i = 0; i < n; i++) {
			int f, dup = 0;
			if (!h[i] || !h[i][0]) continue;
			f = a68_family(h[i]);
			if (f != (pass == 0 ? prefer : pass == 1 ? other : A68_NAME)) continue;
			for (j = 0; j < c; j++) dup |= a68_same(h[order[j]], h[i]);
			if (!dup) order[c++] = i;
		}
	return c;
}

/* ---- the probe's answer ------------------------------------------------------------------------------ */
static long field(const unsigned char *d, long n, long i, char *out, long max)
{
	long k = 0;
	while (i < n && d[i] != '\t' && d[i] != '\r' && d[i] != '\n') {
		if (k < max - 1) out[k++] = (char)d[i];
		i++;
	}
	out[k] = 0;
	return i < n && d[i] == '\t' ? i + 1 : n;
}
int a68_disc_parse(const unsigned char *d, long n, struct a68_disc *r)
{
	static const char head[] = "AURORA68-MUSIC\t";
	char t[16];
	long i;
	r->name[0] = r->v4[0] = r->v6[0] = 0;
	r->port = 0;
	for (i = 0; head[i]; i++)
		if (i >= n || d[i] != (unsigned char)head[i]) return 0;
	i = field(d, n, i, t, sizeof t);                 /* the version: 1 so far */
	if (!a68_is_number(t) || a68_num(t) < 1) return 0;
	i = field(d, n, i, r->name, sizeof r->name);
	i = field(d, n, i, t, sizeof t);
	if (!a68_is_number(t) || a68_num(t) < 1 || a68_num(t) > 65535) return 0;
	r->port = (unsigned)a68_num(t);
	i = field(d, n, i, r->v4, sizeof r->v4);
	field(d, n, i, r->v6, sizeof r->v6);
	if (!a68_is_ipv4(r->v4)) r->v4[0] = 0;
	if (!a68_is_ipv6(r->v6)) r->v6[0] = 0;
	return 1;
}
int a68_disc_hosts(const struct a68_disc *r, const char *sender, int prefer, int can_v6, const char **out)
{
	const char *v4[2], *v6 = can_v6 && r->v6[0] ? r->v6 : 0;
	int n4 = 0, c = 0, i;
	if (sender && sender[0]) v4[n4++] = sender;
	if (r->v4[0] && !(n4 && a68_same(r->v4, sender))) v4[n4++] = r->v4;
	if (v6 && prefer == A68_IPV6) out[c++] = v6;
	for (i = 0; i < n4; i++) out[c++] = v4[i];
	if (v6 && prefer != A68_IPV6) out[c++] = v6;
	return c;
}

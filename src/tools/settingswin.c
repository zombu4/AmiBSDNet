/*
 * AmiBSDNetStatus: the settings window.  Edits the network configuration
 * (ENV:AmiBSDNet/AmiBSDNet.conf, and ENVARC: for Save) and the Wi-Fi
 * network (Wireless.prefs), then has the running stack apply it at once.
 *
 * Ethernet and Wi-Fi are separate interfaces that work at the same time,
 * each with its own address; the default route goes through the first
 * usable interface with a router, in the order of the configuration
 * (src/stack/config.c, stack_update_route()), so merge() puts a new
 * Ethernet line before the Wi-Fi line.  A fixed address stays on the
 * interface that had it (fixed_target()); a new one goes to Ethernet if
 * there is an Ethernet adapter, else to Wi-Fi.
 *
 * write_conf() merges, it does not write the file anew: only the lines
 * this window edits change (hostname, the first Ethernet and the first
 * Wi-Fi interface line, with a fixed address also the gateway and the
 * first nameserver line, autodetect and the PaulaNET verify/CRC lines);
 * every other line (domain, more name servers, more interfaces, words
 * such as "optional", comments) stays as it is.  The syntax is the
 * stack's: src/stack/config.c, parse_line(), parse_interface() and
 * tokenize().
 *
 * The passphrase field shows '*' (psk_edit()).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <intuition/sghooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include <amibsdnet/control.h>
#include <amibsdnet/probe.h>
#include <amibsdnet/wm.h>
#include <amibsdnet/drvcheck.h>
#include <amibsdnet/devopen.h>

#include "statustool.h"

#define	CONF_ENV	"ENV:AmiBSDNet/AmiBSDNet.conf"
#define	CONF_ENVARC	"ENVARC:AmiBSDNet/AmiBSDNet.conf"

struct settings {
	char	eth[64];
	LONG	ethunit;
	char	wifi[64];
	LONG	wifiunit;
	int	fixed;			/* fixed address instead of DHCP */
	int	fixedon;		/* the line that had it: 1 Ethernet,
					   2 Wi-Fi, 0 none */
	char	addr[24];		/* a.b.c.d/prefix */
	char	gateway[16];
	char	dns[16];
	char	hostname[64];
	char	ssid[34];
	char	psk[65];		/* a passphrase, or 64 hex digits */
	int	paulanet;		/* use the PaulaNET adapter (autodetect) */
	int	verify;			/* check its driver (default off) */
	char	crcs[40];		/* accepted driver CRCs, hex, spaces */
	int	paulaline;		/* a hand-written PaulaNET interface */
	int	sershell;		/* Shell on the serial port */
	int	serbaud;		/* index into bauds[] */
};

static struct settings cur, orig;

/* ------------------------------------------------------------------------
 * small string helpers (no C library)
 */

static int
slen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

static void
scpy(char *d, const char *s, int n)
{

	while (--n > 0 && *s)
		*d++ = *s++;
	*d = '\0';
}

static int
seq(const char *a, const char *b)
{

	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int
lc(int c)
{

	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int
seq_nocase(const char *a, const char *b)
{

	while (*a && lc((UBYTE)*a) == lc((UBYTE)*b))
		a++, b++;
	return *a == *b;
}

static int
has_word(const char *s, const char *w)
{
	int i;

	for (; *s; s++) {
		for (i = 0; w[i] && lc((UBYTE)s[i]) == w[i]; i++)
			;
		if (!w[i])
			return 1;
	}
	return 0;
}

static int
looks_wireless(const char *dev)
{

	return has_word(dev, "wifi") || has_word(dev, "wlan") ||
	    has_word(dev, "prism") || has_word(dev, "wireless");
}

static void
put_num(char **pp, ULONG v)
{
	char tmp[12];
	int i = 0;

	do {
		tmp[i++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (i)
		*(*pp)++ = tmp[--i];
}

static int
valid_ip(const char *s, int prefix_ok)
{
	int part, dots = 0, digits;

	for (;;) {
		/* (more than 3 digits is too big: no overflow) */
		for (part = 0, digits = 0; *s >= '0' && *s <= '9'; s++, digits++)
			if (digits < 3)
				part = part * 10 + (*s - '0');
		if (!digits || digits > 3 || part > 255)
			return 0;
		if (*s == '.' && dots < 3) {
			dots++;
			s++;
			continue;
		}
		break;
	}
	if (dots != 3)
		return 0;
	if (*s == '/' && prefix_ok) {
		for (s++, part = 0, digits = 0; *s >= '0' && *s <= '9'; s++,
		    digits++)
			if (digits < 2)
				part = part * 10 + (*s - '0');
		return digits && digits <= 2 && part <= 32 && !*s;
	}
	return !*s;
}

/* "255.255.255.0" -> 24; -1 if it is not a netmask */
static int
mask_prefix(const char *s)
{
	ULONG m = 0, part;
	int i, plen = 0;

	for (i = 0; i < 4; i++) {
		if (*s < '0' || *s > '9')
			return -1;
		for (part = 0; *s >= '0' && *s <= '9'; s++)
			if (part <= 255)
				part = part * 10 + (*s - '0');
		if (part > 255 || (i < 3 && *s++ != '.'))
			return -1;
		m = (m << 8) | part;
	}
	while (m & 0x80000000UL) {
		plen++;
		m <<= 1;
	}
	return m ? -1 : plen;
}

/* ------------------------------------------------------------------------
 * reading and writing the configuration
 */

static int
tokens(char *line, char **tok, int max)
{
	int n = 0;

	while (n < max) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line || *line == '#' || *line == ';' || *line == '\n' ||
		    *line == '\r')
			break;
		tok[n++] = line;
		while (*line && *line != ' ' && *line != '\t' && *line != '\n' &&
		    *line != '\r')
			line++;
		if (!*line)
			break;
		*line++ = '\0';
	}
	return n;
}

/* an "interface <name> <device> <unit> ..." line (config.c: at least 4
   words): which of them the window edits */
#define	IF_NONE		0
#define	IF_ETH		1
#define	IF_WIFI		2
#define	IF_PAULA	3

static int
iface_kind(char **tok, int n)
{
	int i, wifi;

	if (n < 4 || !seq_nocase(tok[0], "interface"))
		return IF_NONE;
	if (amibsdnet_is_paulanet(tok[2]))
		return IF_PAULA;
	wifi = looks_wireless(tok[2]);
	for (i = 4; i < n; i++)
		if (seq_nocase(tok[i], "wifi"))
			wifi = 1;
	return wifi ? IF_WIFI : IF_ETH;
}

static void
load(struct settings *s)
{
	char line[256], *tok[12];
	BPTR fh;
	int n, i, k;

	memset(s, 0, sizeof(*s));
	scpy(s->hostname, "amiga", sizeof(s->hostname));
	s->paulanet = 1;
	if ((fh = Open((CONST_STRPTR)CONF_ENV, MODE_OLDFILE)) == 0)
		fh = Open((CONST_STRPTR)CONF_ENVARC, MODE_OLDFILE);
	if (fh) {
		/* (the size less one for dos V36/V37, which copy one byte
		   more: dos.doc:2146-2150, FGets BUGS) */
		while (FGets(fh, (STRPTR)line, sizeof(line) - 1)) {
			if ((n = tokens(line, tok, 12)) < 2)
				continue;
			if (seq_nocase(tok[0], "autodetect"))
				s->paulanet = !seq_nocase(tok[1], "off");
			else if (seq_nocase(tok[0], "paulanet") && n >= 3) {
				ULONG v;

				if (seq_nocase(tok[1], "verify"))
					s->verify = seq_nocase(tok[2], "on");
				else if (seq_nocase(tok[1], "crc") &&
				    drv_parse_crc(tok[2], &v) &&
				    slen(s->crcs) + 10 < (int)sizeof(s->crcs)) {
					char *p = s->crcs + slen(s->crcs);

					if (p != s->crcs)
						*p++ = ' ';
					drv_fmt_crc(p, v);
				}
			} else if (iface_kind(tok, n) == IF_PAULA) {
				/* written by hand: merge() keeps the line as
				   it is */
				s->paulaline = 1;
			} else if (seq_nocase(tok[0], "hostname"))
				scpy(s->hostname, tok[1], sizeof(s->hostname));
			else if (seq_nocase(tok[0], "gateway"))
				scpy(s->gateway, tok[1], sizeof(s->gateway));
			else if (seq_nocase(tok[0], "nameserver") && !s->dns[0])
				scpy(s->dns, tok[1], sizeof(s->dns));
			else if ((k = iface_kind(tok, n)) == IF_ETH ||
			    k == IF_WIFI) {
				LONG unit = 0;
				const char *p;

				for (p = tok[3]; *p >= '0' && *p <= '9' &&
				    unit < 100000; p++)
					unit = unit * 10 + (*p - '0');
				if (k == IF_WIFI && !s->wifi[0]) {
					scpy(s->wifi, tok[2], sizeof(s->wifi));
					s->wifiunit = unit;
				} else if (k == IF_ETH && !s->eth[0]) {
					scpy(s->eth, tok[2], sizeof(s->eth));
					s->ethunit = unit;
				} else
					continue;
				/* the fixed address the window edits: the
				   first one, and the line it is on */
				if (s->fixed)
					continue;
				for (i = 4; i + 1 < n; i++)
					if (seq_nocase(tok[i], "address")) {
						s->fixed = 1;
						s->fixedon = k;
						scpy(s->addr, tok[i + 1],
						    sizeof(s->addr));
					}
				/* "netmask a.b.c.d" instead of a /prefix: keep
				   the mask when the file is written again */
				for (i = 4; i + 1 < n; i++)
					if (seq_nocase(tok[i], "netmask") &&
					    s->fixed && !has_word(s->addr, "/")) {
						int plen = mask_prefix(tok[i + 1]);

						if (plen >= 0 && slen(s->addr) + 4 <
						    (int)sizeof(s->addr)) {
							char *p = s->addr + slen(s->addr);

							*p++ = '/';
							put_num(&p, (ULONG)plen);
							*p = '\0';
						}
					}
			}
		}
		Close(fh);
	}
	/* a hand-written PaulaNET line is used whatever "autodetect" says
	   (src/stack/config.c: "autodetect" only stops add_detected()) */
	if (s->paulaline)
		s->paulanet = 1;
	wm_get_network(s->ssid, sizeof(s->ssid), s->psk, sizeof(s->psk));
}

/* the new text of the configuration, built in memory */
struct obuf {
	char	*p;
	LONG	len, size;
	int	err;
};

static void
ob_put(struct obuf *o, const char *s, LONG n)
{
	char *np;
	LONG ns;

	if (o->err || n <= 0)
		return;
	if (o->len + n + 1 > o->size) {
		ns = (o->len + n + 1) * 2 + 256;
		if ((np = AllocVec(ns, MEMF_ANY)) == NULL) {
			o->err = 1;
			return;
		}
		if (o->p) {
			CopyMem(o->p, np, o->len);
			FreeVec(o->p);
		}
		o->p = np;
		o->size = ns;
	}
	CopyMem((APTR)s, o->p + o->len, n);
	o->len += n;
	o->p[o->len] = '\0';
}

static void
ob_str(struct obuf *o, const char *s)
{

	ob_put(o, s, slen(s));
}

static void
ob_num(struct obuf *o, ULONG v)
{
	char b[12], *p = b;

	put_num(&p, v);
	ob_put(o, b, p - b);
}

/* a whole file; NULL and *err 0 if there is none, NULL and *err 1 if it
   is there but cannot be read completely (never merged from a part) */
static char *
read_text(const char *name, LONG *lenp, int *err)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
	char *buf;
	LONG size, len = 0, n;

	*err = 0;
	*lenp = 0;
	if (fh == 0) {
		*err = IoErr() != ERROR_OBJECT_NOT_FOUND;
		return NULL;
	}
	if (Seek(fh, 0, OFFSET_END) < 0 ||
	    (size = Seek(fh, 0, OFFSET_BEGINNING)) < 0 || size > 256 * 1024 ||
	    (buf = AllocVec(size + 1, MEMF_ANY)) == NULL) {
		Close(fh);
		*err = 1;
		return NULL;
	}
	while (len < size && (n = Read(fh, buf + len, size - len)) > 0)
		len += n;
	Close(fh);
	if (len != size) {
		FreeVec(buf);
		*err = 1;
		return NULL;
	}
	buf[len] = '\0';
	*lenp = len;
	return buf;
}

/* where the fixed address goes: the interface that had it, else Ethernet
   if there is one, else Wi-Fi; IF_NONE in DHCP mode */
static int
fixed_target(const struct settings *s)
{

	if (!s->fixed)
		return IF_NONE;
	if (s->fixedon == IF_ETH && s->eth[0])
		return IF_ETH;
	if (s->fixedon == IF_WIFI && s->wifi[0])
		return IF_WIFI;
	return s->eth[0] ? IF_ETH : IF_WIFI;
}

/*
 * An interface line: "interface <name> <device> <unit>", then the
 * address words, then every other word of the old line (old, nold; e.g.
 * "optional").  how: 0 the old line's address words as they were, 1
 * "dhcp", 2 "address <a.b.c.d/prefix>".
 */
static void
put_iface(struct obuf *o, const char *name, const char *dev, LONG unit,
    int how, const char *addr, int wifi, char **old, int nold)
{
	int i;

	ob_str(o, "interface  ");
	ob_str(o, name);
	ob_str(o, " ");
	ob_str(o, dev);
	ob_str(o, " ");
	ob_num(o, (ULONG)unit);
	if (how == 2) {
		ob_str(o, " address ");
		ob_str(o, addr);
		if (!has_word(addr, "/"))
			ob_str(o, "/24");
	} else if (how == 1)
		ob_str(o, " dhcp");
	for (i = 4; i < nold; i++) {
		if (seq_nocase(old[i], "wifi"))
			continue;		/* (written below) */
		if (how != 0 && seq_nocase(old[i], "dhcp"))
			continue;
		if (how != 0 && (seq_nocase(old[i], "address") ||
		    seq_nocase(old[i], "netmask"))) {
			i++;			/* and its value */
			continue;
		}
		ob_str(o, " ");
		ob_str(o, old[i]);
	}
	/* ("wifi" marks a driver whose name does not say so, for load();
	   the stack skips the word, config.c parse_interface()) */
	if (wifi && !looks_wireless(dev))
		ob_str(o, " wifi");
	ob_str(o, "\n");
}

/* how an Ethernet or Wi-Fi line gets its address (see put_iface()) */
static int
iface_how(const struct settings *s, int kind, int hasold)
{

	if (kind == fixed_target(s))
		return 2;
	if (kind == s->fixedon)
		return 1;		/* had the fixed address: DHCP now */
	return hasold ? 0 : 1;
}

static void
put_paulanet_prefs(struct obuf *o, const struct settings *s)
{
	const char *p = s->crcs;
	char one[12];
	int i;

	if (s->verify)
		ob_str(o, "paulanet   verify on\n");
	while (*p) {
		while (*p == ' ')
			p++;
		for (i = 0; *p && *p != ' ' && i < 11; i++)
			one[i] = *p++;
		one[i] = '\0';
		if (i) {
			ob_str(o, "paulanet   crc ");
			ob_str(o, one);
			ob_str(o, "\n");
		}
	}
}

#define	MAXTOK		32
#define	MAXNAMES	16

/* one line of base from p: its end (after the line feed) in *ep, its
   first 255 bytes in line (the stack reads lines into 256 bytes and
   ignores longer ones, config.c read_config()), split into words */
static int
line_tokens(const char *p, const char *end, const char **ep, char *line,
    char **tok)
{
	const char *e;
	LONG ll;

	for (e = p; e < end && *e != '\n'; e++)
		;
	if (e < end)
		e++;
	*ep = e;
	for (ll = 0; ll < e - p && ll < 255; ll++)
		line[ll] = p[ll];
	line[ll] = '\0';
	return tokens(line, tok, MAXTOK);
}

/*
 * The settings merged into the configuration text base: the lines the
 * window edits are replaced (the first of their kind; later ones that
 * would override it are left out), all others stay as they are, and what
 * is missing is added.
 */
static void
merge(const char *base, LONG blen, const struct settings *s, struct obuf *o)
{
	const char *p, *e, *end = base + blen;
	char line[256], *tok[MAXTOK], names[MAXNAMES][16], ethname[16],
	    wifiname[16];
	int n, kind, nn = 0, k, i, haswifi = 0;
	int did_host = 0, seen_eth = 0, seen_wifi = 0, did_eth = 0,
	    did_wifi = 0, did_gw = 0, did_dns = 0, did_auto = 0, did_pn = 0;

	/* the interface names in use, and is there a Wi-Fi line */
	for (p = base; p < end; p = e) {
		n = line_tokens(p, end, &e, line, tok);
		if ((kind = iface_kind(tok, n)) == IF_NONE)
			continue;
		if (kind == IF_WIFI)
			haswifi = 1;
		if (nn < MAXNAMES)
			scpy(names[nn++], tok[1], sizeof(names[0]));
	}
	/* free names for new lines ("sana0" ... "sana9", as the stack picks
	   one for PaulaNET, config.c add_detected()) */
	ethname[0] = wifiname[0] = '\0';
	for (k = 0; k < 10 && (!ethname[0] || !wifiname[0]); k++) {
		char nm[8];

		scpy(nm, "sana0", sizeof(nm));
		nm[4] = '0' + k;
		for (i = 0; i < nn && !seq_nocase(names[i], nm); i++)
			;
		if (i < nn)
			continue;
		if (!ethname[0])
			scpy(ethname, nm, sizeof(ethname));
		else
			scpy(wifiname, nm, sizeof(wifiname));
	}

	for (p = base; p < end; p = e) {
		n = line_tokens(p, end, &e, line, tok);
		kind = iface_kind(tok, n);
		if (n >= 2 && seq_nocase(tok[0], "hostname")) {
			if (!did_host) {
				ob_str(o, "hostname   ");
				ob_str(o, s->hostname[0] ? s->hostname : "amiga");
				ob_str(o, "\n");
				did_host = 1;
			}
			continue;
		}
		if (kind == IF_ETH && !seen_eth) {
			seen_eth = 1;
			if (s->eth[0]) {
				put_iface(o, tok[1], s->eth, s->ethunit,
				    iface_how(s, IF_ETH, 1), s->addr, 0, tok, n);
				did_eth = 1;
			}
			/* a new Wi-Fi line right after Ethernet */
			if (!haswifi && s->wifi[0] && !did_wifi && wifiname[0]) {
				put_iface(o, wifiname, s->wifi, s->wifiunit,
				    iface_how(s, IF_WIFI, 0), s->addr, 1, NULL,
				    0);
				did_wifi = 1;
			}
			continue;
		}
		if (kind == IF_WIFI && !seen_wifi) {
			seen_wifi = 1;
			/* a new Ethernet line goes before Wi-Fi (the route) */
			if (s->eth[0] && !did_eth && ethname[0]) {
				put_iface(o, ethname, s->eth, s->ethunit,
				    iface_how(s, IF_ETH, 0), s->addr, 0, NULL,
				    0);
				did_eth = 1;
			}
			if (s->wifi[0]) {
				put_iface(o, tok[1], s->wifi, s->wifiunit,
				    iface_how(s, IF_WIFI, 1), s->addr, 1, tok, n);
				did_wifi = 1;
			}
			continue;
		}
		if (kind == IF_PAULA && !s->paulanet)
			continue;	/* the PaulaNET box is off */
		/* the gateway and the first name server are the window's
		   fixed-address fields: with DHCP they go (the stack uses
		   configured name servers after the DHCP ones,
		   src/stack/config.c stack_update_dns(), so one left from a
		   fixed address would stay in use) */
		if (n >= 2 && seq_nocase(tok[0], "gateway")) {
			/* (one gateway: a later line would override it,
			   config.c parse_line()) */
			if (s->fixed && !did_gw && s->gateway[0]) {
				ob_str(o, "gateway    ");
				ob_str(o, s->gateway);
				ob_str(o, "\n");
			}
			did_gw = 1;
			continue;
		}
		if (n >= 2 && !did_dns && seq_nocase(tok[0], "nameserver")) {
			/* the first one is the window's; more stay */
			if (s->fixed && s->dns[0]) {
				ob_str(o, "nameserver ");
				ob_str(o, s->dns);
				ob_str(o, "\n");
			}
			did_dns = 1;
			continue;
		}
		if (n >= 2 && seq_nocase(tok[0], "autodetect")) {
			if (!did_auto)
				ob_str(o, s->paulanet ? "autodetect on\n" :
				    "autodetect off\n");
			did_auto = 1;
			continue;
		}
		if (n >= 3 && seq_nocase(tok[0], "paulanet") &&
		    (seq_nocase(tok[1], "verify") || seq_nocase(tok[1], "crc"))) {
			if (!did_pn)
				put_paulanet_prefs(o, s);
			did_pn = 1;
			continue;
		}
		/* not the window's: as it is */
		ob_put(o, p, e - p);
		if (e[-1] != '\n')
			ob_str(o, "\n");
	}
	if (!did_host) {
		ob_str(o, "hostname   ");
		ob_str(o, s->hostname[0] ? s->hostname : "amiga");
		ob_str(o, "\n");
	}
	if (s->eth[0] && !did_eth) {
		if (!ethname[0])
			o->err = 1;	/* no free interface name */
		else
			put_iface(o, ethname, s->eth, s->ethunit,
			    iface_how(s, IF_ETH, 0), s->addr, 0, NULL, 0);
	}
	if (s->wifi[0] && !did_wifi) {
		if (!wifiname[0])
			o->err = 1;
		else
			put_iface(o, wifiname, s->wifi, s->wifiunit,
			    iface_how(s, IF_WIFI, 0), s->addr, 1, NULL, 0);
	}
	if (s->fixed && !did_gw && s->gateway[0]) {
		ob_str(o, "gateway    ");
		ob_str(o, s->gateway);
		ob_str(o, "\n");
	}
	if (s->fixed && !did_dns && s->dns[0]) {
		ob_str(o, "nameserver ");
		ob_str(o, s->dns);
		ob_str(o, "\n");
	}
	if (!did_auto && !s->paulanet)
		ob_str(o, "autodetect off\n");
	if (!did_pn)
		put_paulanet_prefs(o, s);
}

/* a file that is not there yet: written, every write checked; Close()
   "might fail depending on buffering and whatever IO must be done to
   close a file being written to" (dos.doc Close) */
static int
new_file(const char *path, const char *data, LONG len)
{
	BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
	int ok;

	if (fh == 0)
		return -1;
	ok = Write(fh, (APTR)data, len) == len;
	if (!Close(fh))
		ok = 0;
	if (!ok) {
		DeleteFile((CONST_STRPTR)path);
		return -1;
	}
	return 0;
}

/* the settings merged into path (or, if path is not there yet, into the
   other copy of the configuration); 0 on success, -1 with path as it
   was */
static int
write_conf(const char *path, const struct settings *s)
{
	const char *other = seq(path, CONF_ENV) ? CONF_ENVARC : CONF_ENV;
	struct obuf o;
	char *base;
	LONG blen;
	int err, exists, r;

	if ((base = read_text(path, &blen, &err)) == NULL && err)
		return -1;
	exists = base != NULL;
	if (base == NULL && (base = read_text(other, &blen, &err)) == NULL &&
	    err)
		return -1;
	memset(&o, 0, sizeof(o));
	if (base == NULL)
		ob_str(&o, "# AmiBSDNet configuration (AmiBSDNet settings "
		    "window; see the AmiBSDNet documentation)\n");
	merge(base ? base : "", base ? blen : 0, s, &o);
	if (base)
		FreeVec(base);
	if (o.err || o.p == NULL) {
		if (o.p)
			FreeVec(o.p);
		return -1;
	}
	r = exists ? amibsdnet_replace_file(path, o.p, o.len) :
	    new_file(path, o.p, o.len);
	FreeVec(o.p);
	return r;
}

/* the CRC field: hex numbers separated by spaces; 0 if all are good */
static int
valid_crcs(const char *p)
{
	char one[12];
	ULONG v;
	int i, n = 0;

	while (*p) {
		while (*p == ' ')
			p++;
		for (i = 0; *p && *p != ' '; p++)
			if (i < 11)
				one[i++] = *p;
		one[i] = '\0';
		if (i && (!drv_parse_crc(one, &v) || ++n > DRV_MAXCRC))
			return -1;
	}
	return 0;
}

static void
mkdirs(int envarc)
{
	BPTR l;

	if ((l = CreateDir((CONST_STRPTR)"ENV:AmiBSDNet")) != 0)
		UnLock(l);
	if (envarc && (l = CreateDir((CONST_STRPTR)"ENVARC:AmiBSDNet")) != 0)
		UnLock(l);
}

/* ------------------------------------------------------------------------
 * the window
 */

enum {
	GID_ETH = 1, GID_ETHUNIT, GID_WIFI, GID_WIFIUNIT, GID_DETECT,
	GID_MODE, GID_ADDR, GID_GW, GID_DNS, GID_HOST, GID_SSID, GID_SCAN,
	GID_PSK, GID_PAULA, GID_VERIFY, GID_CRC, GID_CHECK, GID_SERIAL,
	GID_BAUD, GID_STATUS,
	GID_SAVE, GID_USE, GID_CANCEL, NGADS
};

static struct Gadget *gad[NGADS];
static struct Window *win;

static void busy(int);
static void serviced_delay(LONG);

static const char *modes[] = { "Automatic (DHCP)", "Fixed address", NULL };

/* the serial Shell's speeds (8N1, no handshaking) */
static ULONG bauds[] = { 9600, 19200, 38400, 57600, 115200, 0 };
static const char *baudlabels[] = { "9600 baud", "19200 baud", "38400 baud",
    "57600 baud", "115200 baud", NULL, NULL };
#define	NBAUDS		5
/* a stored speed that is none of these (SerialShell takes 110 to
   1000000, src/tools/serialshell.c:1016, "if (baud < 110 || baud >
   1000000)"): shown as a sixth choice, bauds[NBAUDS], so that it stays
   as it is unless another one is chosen */
static char baudother[20];
static int nbauds = NBAUDS;
#define	BAUD_DEFAULT	1	/* 19200 */
#define	SERVAR		"AmiBSDNet/SerialShell"

/* ENV:AmiBSDNet/SerialShell holds the baud rate while it is switched on */
static void
load_serial(struct settings *s)
{
	char v[16];
	ULONG b = 0;
	int i;

	s->sershell = 0;
	s->serbaud = BAUD_DEFAULT;
	if (GetVar((CONST_STRPTR)SERVAR, (STRPTR)v, sizeof(v),
	    GVF_GLOBAL_ONLY) <= 0)
		return;
	s->sershell = 1;
	for (i = 0; v[i] >= '0' && v[i] <= '9'; i++)
		b = b * 10 + (v[i] - '0');
	for (i = 0; i < nbauds; i++)
		if (bauds[i] == b) {
			s->serbaud = i;
			return;
		}
	if (b >= 110 && b <= 1000000 && nbauds == NBAUDS) {
		char *p = baudother;

		bauds[NBAUDS] = b;
		put_num(&p, b);
		scpy(p, " baud", sizeof(baudother) - (p - baudother));
		baudlabels[NBAUDS] = baudother;
		nbauds = NBAUDS + 1;
		s->serbaud = NBAUDS;
	}
}

static int
serialshell_running(void)
{
	int r;

	Forbid();
	r = FindPort((CONST_STRPTR)"AmiBSDNet.SerialShell") != NULL;
	Permit();
	return r;
}

/* store it, and start, stop or restart the serial Shell to match;
   -1: cannot start or store, -2: the running one does not stop */
static int
apply_serial(int save)
{
	BPTR in, out;
	int i;
	char v[12], *p = v;
	ULONG flags = GVF_GLOBAL_ONLY | (save ? GVF_SAVE_VAR : 0);
	static char cmd[48];

	if (cur.sershell) {
		put_num(&p, bauds[cur.serbaud]);
		*p = '\0';
		if (!SetVar((CONST_STRPTR)SERVAR, (CONST_STRPTR)v, -1, flags))
			return -3;
	} else
		DeleteVar((CONST_STRPTR)SERVAR, flags);
	if (cur.sershell == orig.sershell && (!cur.sershell ||
	    cur.serbaud == orig.serbaud))
		return 0;
	/* off, or another speed: the running one goes first */
	if (orig.sershell && serialshell_running()) {
		/* asynchronous (STOP itself waits up to 5 s), the window
		   kept up meanwhile */
		in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
		out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
		if (SystemTags((CONST_STRPTR)"C:SerialShell STOP", SYS_Input, in,
		    SYS_Output, out, SYS_Asynch, TRUE, TAG_DONE) != 0) {
			if (in)
				Close(in);
			if (out)
				Close(out);
			return -2;
		}
		/* gone only once its Shell has ended (a command that ignores
		   Ctrl-C keeps it): a new one could not start before */
		busy(1);
		for (i = 0; i < 60 && serialshell_running(); i++)
			serviced_delay(10);
		busy(0);
		if (serialshell_running())
			return -2;
	}
	if (cur.sershell) {
		BPTR l = Lock((CONST_STRPTR)"C:SerialShell", ACCESS_READ);

		/* (asynchronous: a missing command would not be noticed) */
		if (l == 0)
			return -1;
		UnLock(l);
		scpy(cmd, "C:SerialShell START BAUD=", sizeof(cmd));
		scpy(cmd + slen(cmd), v, sizeof(cmd) - slen(cmd));
		in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
		out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
		/* (it keeps running: asynchronous) */
		if (SystemTags((CONST_STRPTR)cmd, SYS_Input, in, SYS_Output, out,
		    SYS_Asynch, TRUE, NP_Name, (ULONG)"SerialShell",
		    TAG_DONE) != 0) {
			if (in)
				Close(in);
			if (out)
				Close(out);
			return -1;
		}
	}
	return 0;
}

/* the Preferences busy pointer while the window waits (intuition.doc
   SetWindowPointerA, WA_BusyPointer, V39) */
static void
busy(int on)
{
	struct TagItem t[] = { { WA_BusyPointer, on }, { WA_PointerDelay, on },
	    { TAG_DONE, 0 } };

	if (win)
		SetWindowPointerA(win, t);
}

/*
 * Waiting with the window kept up to date: ticks (1/50 s) in steps of
 * 5, redrawing it when asked.  Input meanwhile is dropped (the busy
 * pointer shows that it is not taken).
 */
static void
serviced_delay(LONG ticks)
{
	struct IntuiMessage *im;

	for (; ticks > 0; ticks -= 5) {
		Delay(5);
		if (win == NULL)
			continue;
		while ((im = GT_GetIMsg(win->UserPort)) != NULL) {
			ULONG cls = im->Class;

			GT_ReplyIMsg(im);
			if (cls == IDCMP_REFRESHWINDOW) {
				GT_BeginRefresh(win);
				GT_EndRefresh(win, TRUE);
			}
		}
	}
}

/*
 * The passphrase field shows a '*' for each character: a GadTools string
 * gadget with an edit hook of its own (gadtools.doc, GTST_EditHook).  The
 * hook gets every key after Intuition has planned its edit in
 * SGWork.WorkBuffer; it keeps the real text in psk_real, following the
 * edit by SGWork.EditOp, and puts '*' into the planned buffer instead
 * of the typed character (intuition/sghooks.h: "Intuition will use the
 * values found in SGWork fields WorkBuffer, NumChars, BufferPos, and
 * LongInt"; "If you clear SGA_USE, the string gadget will be
 * unchanged").  An edit it cannot follow (undo, a paste of several
 * characters) is refused with a beep, so psk_real and the field always
 * agree.
 */
static char psk_real[65];
static struct Hook psk_hook;

ULONG psk_edit(struct Hook *, struct SGWork *, ULONG *)
    __attribute__((used));

ULONG
psk_edit(struct Hook *h, struct SGWork *sgw, ULONG *msg)
{
	LONG oldn = sgw->StringInfo->NumChars, newn = sgw->NumChars;
	LONG pos = sgw->StringInfo->BufferPos, cnt, at, i;
	UWORD c = sgw->Code;
	char *real = h->h_Data;

	if (*msg != SGH_KEY)
		return 0;		/* (SGH_CLICK: Intuition's own) */
	switch (sgw->EditOp) {
	case EO_NOOP:
	case EO_MOVECURSOR:
	case EO_ENTER:
		return 1;
	case EO_INSERTCHAR:		/* one character at the cursor */
		if (newn != oldn + 1 || newn > 64 || pos < 0 || pos > oldn ||
		    c < 0x20 || c > 0x7e)
			break;
		for (i = oldn; i > pos; i--)
			real[i] = real[i - 1];
		real[pos] = c;
		real[newn] = '\0';
		sgw->WorkBuffer[pos] = '*';
		return 1;
	case EO_REPLACECHAR:		/* the one under the cursor */
		if (pos < 0 || pos > oldn || newn > 64 || c < 0x20 || c > 0x7e)
			break;
		real[pos] = c;
		real[newn] = '\0';
		sgw->WorkBuffer[pos] = '*';
		return 1;
	case EO_DELBACKWARD:		/* before the cursor */
	case EO_DELFORWARD:		/* under and after it */
		cnt = oldn - newn;
		at = sgw->EditOp == EO_DELBACKWARD ? pos - cnt : pos;
		if (cnt < 0 || at < 0 || at + cnt > oldn)
			break;
		for (i = at; i + cnt <= oldn; i++)
			real[i] = real[i + cnt];
		return 1;
	case EO_CLEAR:
		real[0] = '\0';
		return 1;
	}
	sgw->Actions = (sgw->Actions & ~SGA_USE) | SGA_BEEP;
	return 1;
}

/* the hook's entry: the C calling code utility/hooks.h shows as
   "_hookEntry" (A0 hook, A2 object, A1 message onto the stack) */
void psk_hook_entry(void);
__asm__(
"	.text\n"
"	.globl	psk_hook_entry\n"
"psk_hook_entry:\n"
"	move.l	%a1,-(%sp)\n"
"	move.l	%a2,-(%sp)\n"
"	move.l	%a0,-(%sp)\n"
"	jsr	psk_edit\n"
"	lea	12(%sp),%sp\n"
"	rts\n");

/* a hook for a string gadget that shows '*': real (65 bytes) gets the
   text (also for the Wi-Fi window, wifiwin.c) */
void
secret_hook_init(struct Hook *h, char *real)
{

	real[0] = '\0';
	h->h_Entry = (ULONG (*)())psk_hook_entry;
	h->h_SubEntry = (ULONG (*)())psk_edit;
	h->h_Data = real;
}

static void
set_attr(int id, ULONG tag, ULONG val)
{
	struct TagItem t[] = { { tag, val }, { TAG_DONE, 0 } };

	GT_SetGadgetAttrsA(gad[id], win, NULL, t);
}

/* the passphrase into the field: psk_real, and one '*' per character */
static void
set_psk(const char *s)
{
	static char stars[65];
	int i;

	scpy(psk_real, s, sizeof(psk_real));
	for (i = 0; psk_real[i]; i++)
		stars[i] = '*';
	stars[i] = '\0';
	set_attr(GID_PSK, GTST_String, (ULONG)stars);
}

static void
status(const char *text)
{

	set_attr(GID_STATUS, GTTX_Text, (ULONG)text);
}

static const char *
gstr(int id)
{

	return (const char *)((struct StringInfo *)gad[id]->SpecialInfo)->Buffer;
}

static LONG
gnum(int id)
{

	return ((struct StringInfo *)gad[id]->SpecialInfo)->LongInt;
}

static void
ghost_address(void)
{

	set_attr(GID_ADDR, GA_Disabled, !cur.fixed);
	set_attr(GID_GW, GA_Disabled, !cur.fixed);
	set_attr(GID_DNS, GA_Disabled, !cur.fixed);
}

static void
ghost_paulanet(void)
{

	set_attr(GID_VERIFY, GA_Disabled, !cur.paulanet);
	set_attr(GID_CRC, GA_Disabled, !cur.paulanet || !cur.verify);
}

/* a keyboard shortcut for a text field: into it (if it is not greyed) */
static void
key_field(int id)
{

	if (!(gad[id]->Flags & GFLG_DISABLED))
		ActivateGadget(gad[id], win, NULL);
}

static int
checked(int id)
{

	return (gad[id]->Flags & GFLG_SELECTED) != 0;
}

/* what DHCP gave the primary interface, shown while in DHCP mode */
static char live_addr[24], live_gw[16], live_dns[16];

static void
fmt_ip(char *b, ULONG a)
{
	int i;

	for (i = 24; i >= 0; i -= 8) {
		put_num(&b, (a >> i) & 255);
		if (i)
			*b++ = '.';
	}
	*b = '\0';
}

static void
get_live(void)
{
	struct NetCtrlMsg *m;
	ULONG i, mask;
	int plen;

	live_addr[0] = live_gw[0] = live_dns[0] = '\0';
	if (stack_cmd(NETCTRL_IFLIST) != 0)
		return;
	m = status_msg();
	if (m->ndns)
		fmt_ip(live_dns, m->dns[0]);
	for (i = 0; i < m->nifaces && i < NETCTRL_MAXIFACES; i++)
		if ((m->ifaces[i].flags & NETIF_UP) && m->ifaces[i].address) {
			char *p;

			fmt_ip(live_addr, m->ifaces[i].address);
			for (plen = 0, mask = m->ifaces[i].netmask; mask &
			    0x80000000UL; mask <<= 1)
				plen++;
			p = live_addr + slen(live_addr);
			*p++ = '/';
			put_num(&p, plen);
			*p = '\0';
			if (m->ifaces[i].gateway)
				fmt_ip(live_gw, m->ifaces[i].gateway);
			break;
		}
}

static void
show_address(void)
{

	set_attr(GID_ADDR, GTST_String, (ULONG)(cur.fixed ? cur.addr :
	    live_addr));
	set_attr(GID_GW, GTST_String, (ULONG)(cur.fixed ? cur.gateway :
	    live_gw));
	set_attr(GID_DNS, GTST_String, (ULONG)(cur.fixed ? cur.dns :
	    live_dns));
	ghost_address();
}

static void
show(void)
{

	set_attr(GID_ETH, GTST_String, (ULONG)cur.eth);
	set_attr(GID_ETHUNIT, GTIN_Number, cur.ethunit);
	set_attr(GID_WIFI, GTST_String, (ULONG)cur.wifi);
	set_attr(GID_WIFIUNIT, GTIN_Number, cur.wifiunit);
	set_attr(GID_MODE, GTCY_Active, cur.fixed);
	set_attr(GID_HOST, GTST_String, (ULONG)cur.hostname);
	set_attr(GID_SSID, GTST_String, (ULONG)cur.ssid);
	set_psk(cur.psk);
	set_attr(GID_PAULA, GTCB_Checked, cur.paulanet);
	set_attr(GID_VERIFY, GTCB_Checked, cur.verify);
	set_attr(GID_CRC, GTST_String, (ULONG)cur.crcs);
	set_attr(GID_SERIAL, GTCB_Checked, cur.sershell);
	set_attr(GID_BAUD, GTCY_Active, cur.serbaud);
	set_attr(GID_BAUD, GA_Disabled, !cur.sershell);
	show_address();
	ghost_paulanet();
}

/* the gadgets into cur */
static void
collect(void)
{

	scpy(cur.eth, gstr(GID_ETH), sizeof(cur.eth));
	cur.ethunit = gnum(GID_ETHUNIT);
	scpy(cur.wifi, gstr(GID_WIFI), sizeof(cur.wifi));
	cur.wifiunit = gnum(GID_WIFIUNIT);
	if (cur.fixed) {	/* in DHCP mode the fields show DHCP's values */
		scpy(cur.addr, gstr(GID_ADDR), sizeof(cur.addr));
		scpy(cur.gateway, gstr(GID_GW), sizeof(cur.gateway));
		scpy(cur.dns, gstr(GID_DNS), sizeof(cur.dns));
	}
	scpy(cur.hostname, gstr(GID_HOST), sizeof(cur.hostname));
	scpy(cur.ssid, gstr(GID_SSID), sizeof(cur.ssid));
	/* (the field shows '*': the text is psk_real, psk_edit()) */
	scpy(cur.psk, psk_real, sizeof(cur.psk));
	cur.paulanet = checked(GID_PAULA);
	cur.verify = checked(GID_VERIFY);
	cur.sershell = checked(GID_SERIAL);
	scpy(cur.crcs, gstr(GID_CRC), sizeof(cur.crcs));
}

/* the PaulaNET driver the stack would load, checked as Save would set it */
static void
check_driver(void)
{
	static char msg[96];
	struct drvprefs p;
	char path[64], hex[9], one[12];
	const char *ver, *s;
	ULONG crc;
	int r, i;

	collect();
	if (valid_crcs(cur.crcs) != 0) {
		status("Accepted CRC: up to 4 numbers of 8 hex digits");
		return;
	}
	p.verify = cur.verify;
	p.ncrc = 0;
	for (s = cur.crcs; *s && p.ncrc < DRV_MAXCRC;) {
		while (*s == ' ')
			s++;
		for (i = 0; *s && *s != ' ' && i < 11; i++)
			one[i] = *s++;
		one[i] = '\0';
		if (i && drv_parse_crc(one, &p.crc[p.ncrc]))
			p.ncrc++;
	}
	if (!drv_paulanet_path(path, sizeof(path))) {
		status("No PaulaNET.device in DEVS:, DEVS:Networks or PaulaNET:");
		return;
	}
	status("Checking the PaulaNET driver...");
	busy(1);
	r = drv_check_paulanet(path, &p, &ver, &crc);
	busy(0);
	drv_fmt_crc(hex, crc);
	if (r == DRV_OK) {
		scpy(msg, "PaulaNET.device ", sizeof(msg));
		scpy(msg + slen(msg), ver, sizeof(msg) - slen(msg));
		scpy(msg + slen(msg), ": official, OK", sizeof(msg) - slen(msg));
	} else if (r == DRV_ACCEPTED) {
		scpy(msg, "PaulaNET.device: accepted CRC, OK", sizeof(msg));
	} else if (r == DRV_UNKNOWN) {
		/* a newer release: offer its CRC */
		scpy(msg, "Unknown version, CRC ", sizeof(msg));
		scpy(msg + slen(msg), hex, sizeof(msg) - slen(msg));
		if (cur.verify && p.ncrc < DRV_MAXCRC &&
		    slen(cur.crcs) + 10 < (int)sizeof(cur.crcs)) {
			char *e = cur.crcs + slen(cur.crcs);

			if (e != cur.crcs)
				*e++ = ' ';
			scpy(e, hex, 9);
			set_attr(GID_CRC, GTST_String, (ULONG)cur.crcs);
			scpy(msg + slen(msg), " (added: Save to accept)",
			    sizeof(msg) - slen(msg));
		} else if (!cur.verify)
			scpy(msg + slen(msg), " (used: verify is off)",
			    sizeof(msg) - slen(msg));
	} else if (r == DRV_MISSING)
		scpy(msg, "PaulaNET.device cannot be found any more", sizeof(msg));
	else
		scpy(msg, "PaulaNET.device is DAMAGED: copy it again from the "
		    "PaulaNET disk", sizeof(msg));
	status(msg);
}

static void
detect(void)
{
	struct probe_adapter a[PROBE_MAX];
	static char msg[96];
	int n, i, ne = 0, nw = 0;

	status("Looking for network adapters...");
	collect();
	busy(1);
	n = probe_adapters(a, PROBE_MAX);
	busy(0);
	/* a field changes only if an adapter of its kind was found: what
	   was typed in the other one stays */
	for (i = 0; i < n; i++)
		if (a[i].wireless && !nw++) {
			scpy(cur.wifi, a[i].device, sizeof(cur.wifi));
			cur.wifiunit = a[i].unit;
		} else if (!a[i].wireless && !ne++) {
			scpy(cur.eth, a[i].device, sizeof(cur.eth));
			cur.ethunit = a[i].unit;
		}
	show();
	if (n == 0 && probe_paulanet[0])
		status("Found: PaulaNET only (used when plugged in)");
	else if (n == 0)
		status("No network adapters found in DEVS:Networks");
	else {
		char *p = msg;

		scpy(p, "Found: ", 8);
		p += 7;
		scpy(p, ne ? "Ethernet" : "", 9);
		p += slen(p);
		if (ne && nw) {
			scpy(p, " and ", 6);
			p += 5;
		}
		scpy(p, nw ? "Wi-Fi" : "", 6);
		status(msg);
	}
}

/* empty, or one word the stack's tokenize() keeps whole */
static int
one_word(const char *s)
{

	if (*s == '#' || *s == ';')
		return 0;
	for (; *s; s++)
		if (*s == ' ' || *s == '\t')
			return 0;
	return 1;
}

/* returns 0 if the fields can be used, else shows why */
static int
check(void)
{

	/* PaulaNET alone is fine: the stack adds it by itself */
	if (!cur.eth[0] && !cur.wifi[0] &&
	    !(cur.paulanet && (probe_paulanet[0] || cur.paulaline))) {
		status("Enter a network driver, or press Detect");
		return -1;
	}
	if (amibsdnet_is_paulanet(cur.eth) || amibsdnet_is_paulanet(cur.wifi)) {
		status("PaulaNET is added by itself: use the PaulaNET box");
		return -1;
	}
	/* one word each: the stack splits a line at blanks, and a word
	   starting with '#' or ';' starts a comment (config.c tokenize()) */
	if (!one_word(cur.eth) || !one_word(cur.wifi)) {
		status("Driver: a name without blanks, e.g. wifipi.device");
		ActivateGadget(gad[one_word(cur.eth) ? GID_WIFI : GID_ETH], win,
		    NULL);
		return -1;
	}
	if (!one_word(cur.hostname)) {
		status("Host name: one word, without blanks");
		ActivateGadget(gad[GID_HOST], win, NULL);
		return -1;
	}
	/* (config.c parse_ulong(): digits only) */
	if ((cur.eth[0] && cur.ethunit < 0) ||
	    (cur.wifi[0] && cur.wifiunit < 0)) {
		status("Unit: 0 or more");
		return -1;
	}
	if (cur.fixed && !cur.eth[0] && !cur.wifi[0]) {
		status("PaulaNET uses DHCP: a fixed address needs a driver above");
		return -1;
	}
	if (cur.fixed && !valid_ip(cur.addr, 1)) {
		status("Address: for example 192.168.1.20/24");
		ActivateGadget(gad[GID_ADDR], win, NULL);
		return -1;
	}
	if (cur.fixed && cur.gateway[0] && !valid_ip(cur.gateway, 0)) {
		status("Gateway: for example 192.168.1.1");
		ActivateGadget(gad[GID_GW], win, NULL);
		return -1;
	}
	if (cur.fixed && cur.dns[0] && !valid_ip(cur.dns, 0)) {
		status("DNS server: for example 192.168.1.1");
		ActivateGadget(gad[GID_DNS], win, NULL);
		return -1;
	}
	/* the rule wm.c writes Wireless.prefs by (wm_passphrase_ok()) */
	if (cur.ssid[0] && cur.psk[0] && !wm_passphrase_ok(cur.psk)) {
		status("Passphrase: 8 to 63 characters or 64 hex digits "
		    "(empty if open)");
		ActivateGadget(gad[GID_PSK], win, NULL);
		return -1;
	}
	if (valid_crcs(cur.crcs) != 0) {
		status("Accepted CRC: up to 4 numbers of 8 hex digits");
		ActivateGadget(gad[GID_CRC], win, NULL);
		return -1;
	}
	return 0;
}

/* write and apply; returns 0 on success */
static int
apply(int save)
{
	int wifi_changed, i, archfail = 0;

	collect();
	if (check() != 0)
		return -1;
	mkdirs(save);
	if (write_conf(CONF_ENV, &cur) != 0) {
		status("Cannot write " CONF_ENV " (it is as it was)");
		return -1;
	}
	/* (the rest is still done, as for Use, and the failure is said at
	   the end, "Used, but cannot write ...") */
	if (save && write_conf(CONF_ENVARC, &cur) != 0)
		archfail = 1;
	/* the fixed address is on that line now */
	cur.fixedon = fixed_target(&cur);
	wifi_changed = !seq(cur.ssid, orig.ssid) || !seq(cur.psk, orig.psk) ||
	    !seq(cur.wifi, orig.wifi) || cur.wifiunit != orig.wifiunit;
	/* a network changed here: its entry written anew.  Unchanged, Save
	   still stores it: after a Use it is in ENV: only (wm_set_network()),
	   and the window shows ENV:'s (load(), wm_get_network()).  That is
	   ENV:'s file copied as it is (wm_save_env()), not an entry built
	   from the name and passphrase here, which would lose the lines a
	   WEP or 802.1X entry has instead of a passphrase */
	if (cur.ssid[0] && (!seq(cur.ssid, orig.ssid) ||
	    !seq(cur.psk, orig.psk))) {
		if (wm_set_network(cur.ssid, cur.psk, save) != 0) {
			status("Cannot write Wireless.prefs");
			return -1;
		}
	} else if (save && wm_save_env() != 0) {
		status("Cannot write ENVARC:Sys/Wireless.prefs");
		return -1;
	}
	if ((i = apply_serial(save)) != 0) {
		status(i == -2 ? "Serial Shell busy: end its command, then "
		    "try again" : i == -3 ? "Cannot store the serial Shell "
		    "setting" : "Cannot start C:SerialShell (is it installed?)");
		return -1;
	}
	/* done: a retry after a later failure must not restart it */
	orig.sershell = cur.sershell;
	orig.serbaud = cur.serbaud;
	status("Applying...");
	/* the stack starts WirelessManager again with the new network
	   (src/stack/wireless.c wm_starter(), for each Wi-Fi interface it
	   brings up) */
	if (wifi_changed && wm_running()) {
		/* (up to 15 s, as long as wm_stop() waits, with the window
		   kept up to date) */
		wm_signal_stop();
		for (i = 0; i < 75 && wm_running(); i++)
			serviced_delay(10);
		if (wm_running()) {
			status("Saved, but WirelessManager does not stop; "
			    "try again");
			return -1;
		}
	}
	if ((i = stack_cmd(NETCTRL_RECONFIG)) == -2) {
		status("Saved, but AmiBSDNet does not answer (see its log "
		    "in SYS:Storage/AmiBSDNet-Logs); reboot");
		return -1;
	}
	if (i != 0) {
		/* not running: the caller starts it and closes the window, so
		   a failure to save is said here, with the window kept (Use
		   then starts it without saving) */
		if (archfail) {
			status("Cannot write " CONF_ENVARC "; Use starts "
			    "AmiBSDNet without saving");
			return -1;
		}
		return 1;
	}
	if (status_msg()->result != 0) {
		status("Saved, but the stack could not apply it (see its "
		    "log in SYS:Storage/AmiBSDNet-Logs); try again");
		return -1;
	}
	CopyMem(&cur, &orig, sizeof(orig));
	if (archfail) {
		status("Used, but cannot write " CONF_ENVARC);
		return -1;
	}
	return 0;
}

static void
scan(void)
{
	struct NetCtrlIface ifc;
	char before_ssid[34], before_psk[65], ssid[34], psk[65];

	collect();
	if (!cur.wifi[0]) {
		status("Enter the Wi-Fi driver first (or press Detect)");
		return;
	}
	memset(&ifc, 0, sizeof(ifc));
	scpy(ifc.device, cur.wifi, sizeof(ifc.device));
	ifc.unit = cur.wifiunit;
	ifc.flags = NETIF_WIRELESS;
	wm_get_network(before_ssid, sizeof(before_ssid), before_psk,
	    sizeof(before_psk));
	wifi_window(&ifc);
	/* the Wi-Fi window stores a chosen network in Wireless.prefs
	   itself: taken over only if it did (else what was typed here
	   stays) */
	wm_get_network(ssid, sizeof(ssid), psk, sizeof(psk));
	if (!seq(ssid, before_ssid) || !seq(psk, before_psk)) {
		scpy(cur.ssid, ssid, sizeof(cur.ssid));
		scpy(cur.psk, psk, sizeof(cur.psk));
		scpy(orig.ssid, ssid, sizeof(orig.ssid));
		scpy(orig.psk, psk, sizeof(orig.psk));
	}
	show();
	status("");
}

static struct Gadget *
mk(ULONG kind, struct Gadget *prev, struct NewGadget *ng, int id,
    const char *label, ULONG flags, ULONG tag1, ULONG val1, ULONG tag2,
    ULONG val2)
{
	struct TagItem t[] = { { GT_Underscore, '_' }, { tag1, val1 },
	    { tag2, val2 }, { TAG_DONE, 0 } };

	ng->ng_GadgetID = id;
	ng->ng_GadgetText = (UBYTE *)label;
	ng->ng_Flags = flags;
	return gad[id] = CreateGadgetA(kind, prev, ng, t);
}

int
settings_window(void)
{
	struct Screen *scr;
	APTR vi;
	struct Gadget *glist = NULL, *g;
	struct NewGadget ng;
	struct IntuiMessage *im;
	int quit = 0, rv = 0, r;
	UWORD fh, top, row, gap, cw, w = 470, lx = 150, h;

	if ((GadToolsBase = OpenLibrary("gadtools.library", 37)) == NULL)
		return 0;
	if ((scr = LockPubScreen(NULL)) == NULL)
		goto out;
	if ((vi = GetVisualInfoA(scr, NULL)) == NULL)
		goto unlock;
	load(&cur);
	load_serial(&cur);
	CopyMem(&cur, &orig, sizeof(orig));
	if (!drv_paulanet_path(probe_paulanet, sizeof(probe_paulanet)))
		probe_paulanet[0] = '\0';

	fh = scr->Font->ta_YSize;
	cw = scr->RastPort.Font ? scr->RastPort.Font->tf_XSize : 8;
	row = fh + 8;
	gap = 6;
	top = scr->WBorTop + fh + 1 + 6;
	/* 13 rows, 4 gaps: on a short screen (PAL 640x256) closer together */
	if (top + 13 * row + 4 * gap + 2 + fh + 6 + scr->WBorBottom + 6 >
	    scr->Height) {
		row = fh + 6;
		gap = 2;
		top = scr->WBorTop + fh + 1 + 3;
	}
	g = create_context(&glist);
	memset(&ng, 0, sizeof(ng));
	ng.ng_TextAttr = scr->Font;
	ng.ng_VisualInfo = vi;
	ng.ng_Height = fh + 6;

#define	STR(id, lbl, max)	g = mk(STRING_KIND, g, &ng, id, lbl, \
				    PLACETEXT_LEFT, GTST_MaxChars, max, \
				    TAG_IGNORE, 0)
	/* adapters */
	ng.ng_LeftEdge = lx;
	ng.ng_TopEdge = top;
	ng.ng_Width = 190;
	STR(GID_ETH, "_Ethernet driver", 63);
	ng.ng_LeftEdge = lx + 190 + 50;
	ng.ng_Width = w - ng.ng_LeftEdge - 10;
	g = mk(INTEGER_KIND, g, &ng, GID_ETHUNIT, "Unit", PLACETEXT_LEFT,
	    GTIN_MaxChars, 2, TAG_IGNORE, 0);
	ng.ng_TopEdge += row;
	ng.ng_LeftEdge = lx;
	ng.ng_Width = 190;
	STR(GID_WIFI, "_Wi-Fi driver", 63);
	ng.ng_LeftEdge = lx + 190 + 50;
	ng.ng_Width = w - ng.ng_LeftEdge - 10;
	g = mk(INTEGER_KIND, g, &ng, GID_WIFIUNIT, "Unit", PLACETEXT_LEFT,
	    GTIN_MaxChars, 2, TAG_IGNORE, 0);
	ng.ng_TopEdge += row;
	ng.ng_LeftEdge = lx;
	ng.ng_Width = w - lx - 10;
	g = mk(BUTTON_KIND, g, &ng, GID_DETECT, "_Detect network adapters",
	    PLACETEXT_IN, TAG_IGNORE, 0, TAG_IGNORE, 0);

	/* address */
	ng.ng_TopEdge += row + gap;
	g = mk(CYCLE_KIND, g, &ng, GID_MODE, "_Address", PLACETEXT_LEFT,
	    GTCY_Labels, (ULONG)modes, GTCY_Active, cur.fixed);
	ng.ng_TopEdge += row;
	STR(GID_ADDR, "_IP address/prefix", 23);
	ng.ng_TopEdge += row;
	STR(GID_GW, "_Gateway", 15);
	ng.ng_TopEdge += row;
	STR(GID_DNS, "D_NS server", 15);
	ng.ng_TopEdge += row;
	STR(GID_HOST, "_Host name", 63);

	/* Wi-Fi network */
	ng.ng_TopEdge += row + gap;
	ng.ng_Width = w - lx - 10 - 90;
	STR(GID_SSID, "Wi-Fi ne_twork", 32);
	ng.ng_LeftEdge = w - 10 - 84;
	ng.ng_Width = 84;
	g = mk(BUTTON_KIND, g, &ng, GID_SCAN, "S_can...", PLACETEXT_IN,
	    TAG_IGNORE, 0, TAG_IGNORE, 0);
	ng.ng_TopEdge += row;
	ng.ng_LeftEdge = lx;
	ng.ng_Width = w - lx - 10;
	/* (64: a raw key of 64 hex digits fits too; shown as '*') */
	secret_hook_init(&psk_hook, psk_real);
	g = mk(STRING_KIND, g, &ng, GID_PSK, "_Passphrase", PLACETEXT_LEFT,
	    GTST_MaxChars, 64, GTST_EditHook, (ULONG)&psk_hook);

	/* PaulaNET (Wi-Fi through the floppy port), all on one row */
	ng.ng_TopEdge += row + gap;
	ng.ng_LeftEdge = 10;
	ng.ng_Width = 26;
	g = mk(CHECKBOX_KIND, g, &ng, GID_PAULA, "PaulaNET",
	    PLACETEXT_RIGHT, GTCB_Checked, cur.paulanet, GTCB_Scaled, TRUE);
	ng.ng_LeftEdge = 10 + 26 + 4 + 8 * cw + 10;
	g = mk(CHECKBOX_KIND, g, &ng, GID_VERIFY, "Verify",
	    PLACETEXT_RIGHT, GTCB_Checked, cur.verify, GTCB_Scaled, TRUE);
	ng.ng_LeftEdge += 26 + 4 + 6 * cw + 10 + 4 * cw;
	/* (a wide screen font: keep the field usable) */
	if (ng.ng_LeftEdge > w - 10 - 70 - 6 - 60)
		ng.ng_LeftEdge = w - 10 - 70 - 6 - 60;
	ng.ng_Width = w - 10 - 70 - 6 - ng.ng_LeftEdge;
	STR(GID_CRC, "CRC", 39);
	ng.ng_LeftEdge = w - 10 - 70;
	ng.ng_Width = 70;
	g = mk(BUTTON_KIND, g, &ng, GID_CHECK, "Check", PLACETEXT_IN,
	    TAG_IGNORE, 0, TAG_IGNORE, 0);

	/* a Shell on the serial port (C:SerialShell): 8N1, no handshaking */
	ng.ng_TopEdge += row;
	ng.ng_LeftEdge = 10;
	ng.ng_Width = 26;
	g = mk(CHECKBOX_KIND, g, &ng, GID_SERIAL, "Shell on the serial port",
	    PLACETEXT_RIGHT, GTCB_Checked, cur.sershell, GTCB_Scaled, TRUE);
	ng.ng_LeftEdge = w - 10 - 150;
	ng.ng_Width = 150;
	g = mk(CYCLE_KIND, g, &ng, GID_BAUD, NULL, 0, GTCY_Labels,
	    (ULONG)baudlabels, GTCY_Active, cur.serbaud);

	/* status line and buttons */
	ng.ng_TopEdge += row + gap;
	ng.ng_LeftEdge = 10;
	ng.ng_Width = w - 20;
	g = mk(TEXT_KIND, g, &ng, GID_STATUS, NULL, 0, GTTX_Border, TRUE,
	    GTTX_Text, (ULONG)"");
	ng.ng_TopEdge += row + 2;
	ng.ng_Width = (w - 40) / 3;
	g = mk(BUTTON_KIND, g, &ng, GID_SAVE, "_Save", PLACETEXT_IN,
	    TAG_IGNORE, 0, TAG_IGNORE, 0);
	ng.ng_LeftEdge += ng.ng_Width + 10;
	g = mk(BUTTON_KIND, g, &ng, GID_USE, "_Use", PLACETEXT_IN,
	    TAG_IGNORE, 0, TAG_IGNORE, 0);
	ng.ng_LeftEdge += ng.ng_Width + 10;
	g = mk(BUTTON_KIND, g, &ng, GID_CANCEL, "Cance_l", PLACETEXT_IN,
	    TAG_IGNORE, 0, TAG_IGNORE, 0);
#undef	STR
	h = ng.ng_TopEdge + ng.ng_Height + scr->WBorBottom + 6;
	if (g == NULL)
		goto freegads;

	{
		struct TagItem t[] = {
			{ WA_Title, (ULONG)"AmiBSDNet Settings" },
			{ WA_Width, w }, { WA_Height, h },
			{ WA_Gadgets, (ULONG)glist },
			{ WA_PubScreen, (ULONG)scr },
			{ WA_DragBar, TRUE }, { WA_DepthGadget, TRUE },
			{ WA_CloseGadget, TRUE }, { WA_Activate, TRUE },
			{ WA_AutoAdjust, TRUE },
			{ WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			    IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE |
			    IDCMP_VANILLAKEY | IDCMP_INTUITICKS },
			{ TAG_DONE, 0 }
		};

		if ((win = OpenWindowTagList(NULL, t)) == NULL)
			goto freegads;
	}
	GT_RefreshWindow(win, NULL);
	/* what DHCP gave, asked with the window up (stack_cmd() waits for
	   the stack's answer, status.c) */
	status("Asking the stack...");
	busy(1);
	get_live();
	busy(0);
	status("");
	show();
	if (!cur.eth[0] && !cur.wifi[0] &&
	    !(cur.paulanet && (probe_paulanet[0] || cur.paulaline)))
		detect();
	else
		status("Ethernet and Wi-Fi can be used at the same time");

	while (!quit) {
		WaitPort(win->UserPort);
		while ((im = GT_GetIMsg(win->UserPort)) != NULL) {
			ULONG cls = im->Class;
			UWORD code = im->Code;
			struct Gadget *gd = (struct Gadget *)im->IAddress;
			int id = 0;

			GT_ReplyIMsg(im);
			switch (cls) {
			case IDCMP_CLOSEWINDOW:
				quit = 1;
				break;
			case IDCMP_REFRESHWINDOW:
				GT_BeginRefresh(win);
				GT_EndRefresh(win, TRUE);
				break;
			case IDCMP_VANILLAKEY:
				/* the underlined letters */
				if (code == 27) {
					quit = 1;
					break;
				}
				switch (lc(code)) {
				case 'e': key_field(GID_ETH); break;
				case 'w': key_field(GID_WIFI); break;
				case 'i': key_field(GID_ADDR); break;
				case 'g': key_field(GID_GW); break;
				case 'n': key_field(GID_DNS); break;
				case 'h': key_field(GID_HOST); break;
				case 't': key_field(GID_SSID); break;
				case 'p': key_field(GID_PSK); break;
				case 'd': id = GID_DETECT; break;
				case 'c': id = GID_SCAN; break;
				case 's': id = GID_SAVE; break;
				case 'u': id = GID_USE; break;
				case 'l': id = GID_CANCEL; break;
				case 'a':
					/* the cycle gadget: to the other mode */
					collect();
					code = !cur.fixed;
					set_attr(GID_MODE, GTCY_Active, code);
					id = GID_MODE;
					break;
				}
				break;
			case IDCMP_GADGETUP:
				id = gd->GadgetID;
				break;
			}
			switch (id) {
			case GID_DETECT:
				detect();
				break;
			case GID_MODE:
				collect();
				cur.fixed = code;
				/* switching to fixed: start from DHCP's address */
				if (cur.fixed && !cur.addr[0]) {
					scpy(cur.addr, live_addr, sizeof(cur.addr));
					scpy(cur.gateway, live_gw,
					    sizeof(cur.gateway));
					if (!cur.dns[0])
						scpy(cur.dns, live_dns,
						    sizeof(cur.dns));
				}
				show_address();
				if (cur.fixed)
					ActivateGadget(gad[GID_ADDR], win, NULL);
				break;
			case GID_SCAN:
				scan();
				break;
			case GID_PAULA:
			case GID_VERIFY:
				collect();
				ghost_paulanet();
				break;
			case GID_SERIAL:
				collect();
				set_attr(GID_BAUD, GA_Disabled, !cur.sershell);
				break;
			case GID_BAUD:
				cur.serbaud = (int)code < nbauds ? (int)code :
				    BAUD_DEFAULT;
				break;
			case GID_CHECK:
				check_driver();
				break;
			case GID_SAVE:
			case GID_USE:
				r = apply(id == GID_SAVE);
				if (r >= 0) {
					rv = r == 1 ? 2 : 1;
					quit = 1;
				}
				break;
			case GID_CANCEL:
				quit = 1;
				break;
			}
		}
	}
	CloseWindow(win);
	win = NULL;
freegads:
	FreeGadgets(glist);
	FreeVisualInfo(vi);
unlock:
	UnlockPubScreen(NULL, scr);
out:
	CloseLibrary(GadToolsBase);
	return rv;
}

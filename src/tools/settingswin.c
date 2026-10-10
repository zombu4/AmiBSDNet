/*
 * AmiBSDNetStatus: the settings window.  Edits the network configuration
 * (ENV:AmiBSDNet/AmiBSDNet.conf, and ENVARC: for Save) and the Wi-Fi
 * network (Wireless.prefs), then has the running stack apply it at once.
 *
 * Ethernet and Wi-Fi are separate interfaces that work at the same time,
 * each with its own address; the default route uses Ethernet while it is
 * connected and Wi-Fi otherwise.  A fixed address applies to Ethernet if
 * there is an Ethernet adapter, else to Wi-Fi; the other one uses DHCP.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <utility/tagitem.h>
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
	char	addr[24];		/* a.b.c.d/prefix */
	char	gateway[16];
	char	dns[16];
	char	hostname[64];
	char	ssid[34];
	char	psk[64];
	int	paulanet;		/* use the PaulaNET adapter (autodetect) */
	int	verify;			/* check its driver (default off) */
	char	crcs[40];		/* accepted driver CRCs, hex, spaces */
	char	paulaline[200];		/* a hand-written PaulaNET interface */
	int	paulafixed;		/* ... with a fixed address */
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
		for (part = 0, digits = 0; *s >= '0' && *s <= '9'; s++, digits++)
			part = part * 10 + (*s - '0');
		if (!digits || part > 255)
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
			part = part * 10 + (*s - '0');
		return digits && part <= 32 && !*s;
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

static void
load(struct settings *s)
{
	char line[256], *tok[12];
	BPTR fh;
	int n, i;

	memset(s, 0, sizeof(*s));
	scpy(s->hostname, "amiga", sizeof(s->hostname));
	s->paulanet = 1;
	if ((fh = Open((CONST_STRPTR)CONF_ENV, MODE_OLDFILE)) == 0)
		fh = Open((CONST_STRPTR)CONF_ENVARC, MODE_OLDFILE);
	if (fh) {
		while (FGets(fh, (STRPTR)line, sizeof(line))) {
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
			} else if (seq_nocase(tok[0], "interface") && n >= 4 &&
			    amibsdnet_is_paulanet(tok[2])) {
				/* written by hand: kept as it is, except for the
				   interface name (write_conf() picks a free one) */
				char *p = s->paulaline;

				for (i = 2; i < n && p - s->paulaline <
				    (int)sizeof(s->paulaline) - 4; i++) {
					if (i > 2)
						*p++ = ' ';
					scpy(p, tok[i], sizeof(s->paulaline) - 2 -
					    (p - s->paulaline));
					p += slen(p);
					/* a fixed address: its gateway and name
					   server lines must be written again */
					if (seq_nocase(tok[i], "address"))
						s->paulafixed = 1;
				}
			} else if (seq_nocase(tok[0], "hostname"))
				scpy(s->hostname, tok[1], sizeof(s->hostname));
			else if (seq_nocase(tok[0], "gateway"))
				scpy(s->gateway, tok[1], sizeof(s->gateway));
			else if (seq_nocase(tok[0], "nameserver") && !s->dns[0])
				scpy(s->dns, tok[1], sizeof(s->dns));
			else if (seq_nocase(tok[0], "interface") && n >= 4) {
				int wifi = looks_wireless(tok[2]);
				LONG unit = 0;
				const char *p;

				for (i = 4; i < n; i++)
					if (seq_nocase(tok[i], "wifi"))
						wifi = 1;
				for (p = tok[3]; *p >= '0' && *p <= '9'; p++)
					unit = unit * 10 + (*p - '0');
				if (wifi && !s->wifi[0]) {
					scpy(s->wifi, tok[2], sizeof(s->wifi));
					s->wifiunit = unit;
				} else if (!wifi && !s->eth[0]) {
					scpy(s->eth, tok[2], sizeof(s->eth));
					s->ethunit = unit;
				} else
					continue;
				for (i = 4; i + 1 < n; i++)
					if (seq_nocase(tok[i], "address")) {
						s->fixed = 1;
						scpy(s->addr, tok[i + 1],
						    sizeof(s->addr));
					}
				/* "netmask a.b.c.d" instead of a /prefix: keep
				   the mask when the file is written again */
				for (i = 4; i + 1 < n; i++)
					if (seq_nocase(tok[i], "netmask") &&
					    s->fixed && !has_word(s->addr, "/")) {
						int plen = mask_prefix(tok[i + 1]);

						if (plen >= 0) {
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
	/* a hand-written PaulaNET line is used whatever "autodetect" says */
	if (s->paulaline[0])
		s->paulanet = 1;
	wm_get_network(s->ssid, sizeof(s->ssid), s->psk, sizeof(s->psk));
}

static void
put_iface(BPTR fh, const char *name, const char *dev, LONG unit, int fixed,
    const char *addr, int wifi)
{
	char line[200], *p = line;

	scpy(p, "interface  ", 12);
	p += slen(p);
	scpy(p, name, 8);
	p += slen(p);
	*p++ = ' ';
	scpy(p, dev, 64);
	p += slen(p);
	*p++ = ' ';
	put_num(&p, (ULONG)unit);
	if (fixed) {
		scpy(p, " address ", 10);
		p += slen(p);
		scpy(p, addr, 24);
		p += slen(p);
		if (!has_word(addr, "/")) {
			scpy(p, "/24", 4);
			p += 3;
		}
	} else {
		scpy(p, " dhcp", 6);
		p += 5;
	}
	if (wifi) {
		scpy(p, " wifi", 6);
		p += 5;
	}
	*p++ = '\n';
	*p = '\0';
	FPuts(fh, (CONST_STRPTR)line);
}

static int
write_conf(const char *path, const struct settings *s)
{
	BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
	int n = 0, fixed_eth = s->fixed && s->eth[0], fixed;
	char name[8];

	if (fh == 0)
		return -1;
	FPuts(fh, (CONST_STRPTR)"# AmiBSDNet configuration (AmiBSDNet "
	    "settings window; see the AmiBSDNet documentation)\n");
	FPuts(fh, (CONST_STRPTR)"hostname   ");
	FPuts(fh, (CONST_STRPTR)(s->hostname[0] ? s->hostname : "amiga"));
	FPuts(fh, (CONST_STRPTR)"\n");
	scpy(name, "sana0", sizeof(name));
	if (s->eth[0]) {
		put_iface(fh, name, s->eth, s->ethunit, fixed_eth, s->addr, 0);
		name[4]++;
		n++;
	}
	if (s->wifi[0]) {
		put_iface(fh, name, s->wifi, s->wifiunit,
		    s->fixed && !fixed_eth, s->addr, 1);
		name[4]++;
		n++;
	}
	fixed = s->fixed || (s->paulafixed && s->paulaline[0] && s->paulanet);
	if (fixed && s->gateway[0]) {
		FPuts(fh, (CONST_STRPTR)"gateway    ");
		FPuts(fh, (CONST_STRPTR)s->gateway);
		FPuts(fh, (CONST_STRPTR)"\n");
	}
	if (fixed && s->dns[0]) {
		FPuts(fh, (CONST_STRPTR)"nameserver ");
		FPuts(fh, (CONST_STRPTR)s->dns);
		FPuts(fh, (CONST_STRPTR)"\n");
	}
	/* PaulaNET (floppy-port Wi-Fi): found by the stack by itself */
	if (s->paulaline[0] && s->paulanet) {
		FPuts(fh, (CONST_STRPTR)"interface  ");
		FPuts(fh, (CONST_STRPTR)name);
		FPuts(fh, (CONST_STRPTR)" ");
		FPuts(fh, (CONST_STRPTR)s->paulaline);
		FPuts(fh, (CONST_STRPTR)"\n");
	}
	if (!s->paulanet)
		FPuts(fh, (CONST_STRPTR)"autodetect off\n");
	if (s->verify)
		FPuts(fh, (CONST_STRPTR)"paulanet   verify on\n");
	{
		const char *p = s->crcs;
		char one[12];
		int i;

		while (*p) {
			while (*p == ' ')
				p++;
			for (i = 0; *p && *p != ' ' && i < 11; i++)
				one[i] = *p++;
			one[i] = '\0';
			if (i) {
				FPuts(fh, (CONST_STRPTR)"paulanet   crc ");
				FPuts(fh, (CONST_STRPTR)one);
				FPuts(fh, (CONST_STRPTR)"\n");
			}
		}
	}
	Close(fh);
	return 0;
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
	GID_PSK, GID_PAULA, GID_VERIFY, GID_CRC, GID_CHECK, GID_STATUS,
	GID_SAVE, GID_USE, GID_CANCEL, NGADS
};

static struct Gadget *gad[NGADS];
static struct Window *win;

static const char *modes[] = { "Automatic (DHCP)", "Fixed address", NULL };

static void
set_attr(int id, ULONG tag, ULONG val)
{
	struct TagItem t[] = { { tag, val }, { TAG_DONE, 0 } };

	GT_SetGadgetAttrsA(gad[id], win, NULL, t);
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
	set_attr(GID_PSK, GTST_String, (ULONG)cur.psk);
	set_attr(GID_PAULA, GTCB_Checked, cur.paulanet);
	set_attr(GID_VERIFY, GTCB_Checked, cur.verify);
	set_attr(GID_CRC, GTST_String, (ULONG)cur.crcs);
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
	scpy(cur.psk, gstr(GID_PSK), sizeof(cur.psk));
	cur.paulanet = checked(GID_PAULA);
	cur.verify = checked(GID_VERIFY);
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
	r = drv_check_paulanet(path, &p, &ver, &crc);
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
	n = probe_adapters(a, PROBE_MAX);
	cur.eth[0] = cur.wifi[0] = '\0';
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

/* returns 0 if the fields can be used, else shows why */
static int
check(void)
{

	/* PaulaNET alone is fine: the stack adds it by itself */
	if (!cur.eth[0] && !cur.wifi[0] &&
	    !(cur.paulanet && (probe_paulanet[0] || cur.paulaline[0]))) {
		status("Enter a network driver, or press Detect");
		return -1;
	}
	if (amibsdnet_is_paulanet(cur.eth) || amibsdnet_is_paulanet(cur.wifi)) {
		status("PaulaNET is added by itself: use the PaulaNET box");
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
	if (cur.ssid[0] && cur.psk[0] && (slen(cur.psk) < 8 ||
	    slen(cur.psk) > 63)) {
		status("Passphrase: 8 to 63 characters (empty if open)");
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
	int wifi_changed;

	collect();
	if (check() != 0)
		return -1;
	mkdirs(save);
	if (write_conf(CONF_ENV, &cur) != 0 ||
	    (save && write_conf(CONF_ENVARC, &cur) != 0)) {
		status("Cannot write the configuration");
		return -1;
	}
	wifi_changed = !seq(cur.ssid, orig.ssid) || !seq(cur.psk, orig.psk) ||
	    !seq(cur.wifi, orig.wifi) || cur.wifiunit != orig.wifiunit;
	if (cur.ssid[0] && (!seq(cur.ssid, orig.ssid) ||
	    !seq(cur.psk, orig.psk))) {
		if (wm_set_network(cur.ssid, cur.psk) != 0) {
			status("Cannot write Wireless.prefs");
			return -1;
		}
	}
	status("Applying...");
	/* the stack starts WirelessManager again with the new network */
	if (wifi_changed && wm_running() && wm_stop() != 0) {
		status("Saved, but WirelessManager does not stop; try again");
		return -1;
	}
	if (stack_cmd(NETCTRL_RECONFIG) != 0)
		return 1;		/* not running: the caller starts it */
	if (status_msg()->result != 0) {
		status("Saved, but the stack could not apply it (see "
		    "T:AmiBSDNet.log); try again");
		return -1;
	}
	CopyMem(&cur, &orig, sizeof(orig));
	return 0;
}

static void
scan(void)
{
	struct NetCtrlIface ifc;

	collect();
	if (!cur.wifi[0]) {
		status("Enter the Wi-Fi driver first (or press Detect)");
		return;
	}
	memset(&ifc, 0, sizeof(ifc));
	scpy(ifc.device, cur.wifi, sizeof(ifc.device));
	ifc.unit = cur.wifiunit;
	ifc.flags = NETIF_WIRELESS;
	wifi_window(&ifc);
	/* the Wi-Fi window stores the chosen network itself */
	wm_get_network(cur.ssid, sizeof(cur.ssid), cur.psk, sizeof(cur.psk));
	orig.ssid[0] = '\0';
	scpy(orig.ssid, cur.ssid, sizeof(orig.ssid));
	scpy(orig.psk, cur.psk, sizeof(orig.psk));
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
	CopyMem(&cur, &orig, sizeof(orig));
	get_live();
	if (!drv_paulanet_path(probe_paulanet, sizeof(probe_paulanet)))
		probe_paulanet[0] = '\0';

	fh = scr->Font->ta_YSize;
	cw = scr->RastPort.Font ? scr->RastPort.Font->tf_XSize : 8;
	row = fh + 8;
	gap = 6;
	top = scr->WBorTop + fh + 1 + 6;
	/* 12 rows, 4 gaps: on a short screen (PAL 640x256) closer together */
	if (top + 12 * row + 4 * gap + 2 + fh + 6 + scr->WBorBottom + 6 >
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
	STR(GID_PSK, "_Passphrase", 63);

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
	show();
	if (!cur.eth[0] && !cur.wifi[0] &&
	    !(cur.paulanet && (probe_paulanet[0] || cur.paulaline[0])))
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

/*
 * AmiBSDNet: configuration file parsing and interface setup.
 */

#include <exec/types.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <exec/execbase.h>
#include <proto/dos.h>

#include "rumpuser_amiga.h"
#include "sana2_host.h"
#include "stack.h"
#include <amibsdnet/drvcheck.h>
#include <amibsdnet/devopen.h>

#define	P	amiga_rump_printf

/* the resolver's own default (src/lib/netdb.c:34), set again when a new
   configuration has no hostname line */
#define	DEFAULT_HOSTNAME	"amiga"

extern struct ExecBase *SysBase;

struct iface ifaces[MAX_IFACES];
int nifaces;
struct Task *volatile stack_task;

static ULONG cfg_ns[4];
static int cfg_nns;
static char cfg_domain[128];	/* "domain" of the configuration */
static ULONG cfg_gateway;
static char cfgfile[256], cfgfallback[256];	/* ENV: and ENVARC: copies */

/* bumped on every (re)configuration; DHCP clients of an older
   generation stop */
volatile ULONG config_generation;

/* a configuration file, read but not yet applied */
struct config {
	struct iface ifs[MAX_IFACES];
	int n;
	ULONG ns[4];
	int nns;
	ULONG gateway;
	char hostname[128];		/* as big as netdb.c's hostname_cfg[] */
	char domain[128];		/* (netdb.c's domainname[] is 256: a
					   longer line is refused here) */
	int autodetect;
	struct drvprefs drv;
};

/*
 * All interfaces work at the same time, each with its own address and
 * its own network.  The default route goes through the first usable
 * interface that has a router, in configuration order (the installer
 * lists Ethernet before Wi-Fi), so plugging in a cable takes over from
 * Wi-Fi and unplugging it falls back.  Called by the DHCP clients and
 * the stack task.
 */
static struct SignalSemaphore routesem;
static int routesem_ready;
static ULONG route_gw, route_ifa;
static ULONG fail_gw, fail_ifa;		/* the last route that failed */

void
stack_update_route(void)
{
	ULONG gw = 0, ifa = 0;
	int i;

	ObtainSemaphore(&routesem);
	for (i = 0; i < nifaces; i++)
		if (ifaces[i].up && ifaces[i].gateway) {
			gw = ifaces[i].gateway;
			/* the interface only if the router is on its network
			   (one "gateway" line is shared by every static
			   interface; the kernel then finds the right one) */
			if (ifaces[i].dhcp ||
			    ((gw ^ ifaces[i].addr) & ifaces[i].mask) == 0)
				ifa = ifaces[i].addr;	/* (DHCP: its own router,
							   also off the subnet) */
			break;
		}
	/* also when only the interface changes: Ethernet and Wi-Fi on the
	   same network have the same router */
	if (gw != route_gw || ifa != route_ifa) {
		if (route_gw)
			rump_amibsdnet_route4(NB_RTM_DELETE, 0, 0, route_gw, 0);
		route_gw = route_ifa = 0;
		if (gw) {
			if (rump_amibsdnet_route4(NB_RTM_ADD, 0, 0, gw, ifa) == 0) {
				route_gw = gw;
				route_ifa = ifa;
				fail_gw = fail_ifa = 0;
				P("default route via %lu.%lu.%lu.%lu\n", gw >> 24,
				    (gw >> 16) & 255, (gw >> 8) & 255, gw & 255);
			} else if (gw != fail_gw || ifa != fail_ifa) {
				/* tried again at the next change; said once */
				fail_gw = gw;
				fail_ifa = ifa;
				P("default route via %lu.%lu.%lu.%lu: errno %d\n",
				    gw >> 24, (gw >> 16) & 255, (gw >> 8) & 255,
				    gw & 255, amiga_rump_errno());
			}
		}
	}
	ReleaseSemaphore(&routesem);
}

/* name servers of all usable interfaces (primary first), then the
   configured ones */
void
stack_update_dns(void)
{
	ULONG all[4];
	int i, j, k, n = 0;

	ObtainSemaphore(&routesem);
	for (i = 0; i <= nifaces; i++) {
		const ULONG *list = i < nifaces ? ifaces[i].dns : cfg_ns;
		int cnt = i < nifaces ? (ifaces[i].up ? ifaces[i].ndns : 0) :
		    cfg_nns;

		for (j = 0; j < cnt && n < 4; j++) {
			for (k = 0; k < n && all[k] != list[j]; k++)
				;
			if (k == n)
				all[n++] = list[j];
		}
	}
	/* also when none are left: the old network's servers are gone */
	netdb_set_nameservers(all, n);
	ReleaseSemaphore(&routesem);
}

/* the default domain: the first usable interface's from DHCP
   (src/stack/dhcp.c update_lease()), else the configuration's (also when
   that is empty: a lease that is gone or a network without option 15
   does not leave the old network's domain) */
void
stack_update_domain(void)
{
	const char *dom = cfg_domain;
	int i;

	ObtainSemaphore(&routesem);
	for (i = 0; i < nifaces; i++)
		if (ifaces[i].up && ifaces[i].domain[0]) {
			dom = ifaces[i].domain;
			break;
		}
	netdb_set_domain(dom);
	ReleaseSemaphore(&routesem);
}

void
sb_copy(char *d, const char *s, unsigned long n)
{
	unsigned long i;

	if (n == 0)
		return;
	for (i = 0; i + 1 < n && s[i]; i++)
		d[i] = s[i];
	d[i] = '\0';
}

static unsigned long
slen(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

static int
streq(const char *a, const char *b)
{
	int ca, cb;

	do {
		ca = (UBYTE)*a++;
		cb = (UBYTE)*b++;
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
	} while (ca && ca == cb);
	return ca == cb;
}

static int
parse_ulong(const char *s, ULONG *v)
{
	ULONG n = 0;

	if (*s < '0' || *s > '9')
		return 0;
	while (*s >= '0' && *s <= '9') {
		if (n > (0xffffffffUL - (*s - '0')) / 10)
			return 0;
		n = n * 10 + (*s++ - '0');
	}
	if (*s)
		return 0;
	*v = n;
	return 1;
}

/* "a.b.c.d" or "a.b.c.d/prefix"; mask set only if a prefix is given */
static int
parse_ip(const char *s, ULONG *addr, ULONG *mask)
{
	ULONG v = 0, part;
	int i, digits;

	for (i = 0; i < 4; i++) {
		if (*s < '0' || *s > '9')
			return 0;
		for (part = 0, digits = 0; *s >= '0' && *s <= '9'; s++)
			if (++digits <= 3)
				part = part * 10 + (*s - '0');
		if (digits > 3 || part > 255)
			return 0;
		v = (v << 8) | part;
		if (i < 3 && *s++ != '.')
			return 0;
	}
	/* nothing is stored unless the whole text is valid */
	if (*s == '/' && mask) {
		ULONG plen;

		if (!parse_ulong(s + 1, &plen) || plen > 32)
			return 0;
		*addr = v;
		*mask = plen ? 0xffffffffUL << (32 - plen) : 0;
		return 1;
	}
	if (*s != '\0')
		return 0;
	*addr = v;
	return 1;
}

/* contiguous ones from the top, at least one */
static int
mask_ok(ULONG m)
{
	ULONG inv = ~m;

	return m != 0 && (inv & (inv + 1)) == 0;
}

/* the classful mask NetBSD gives an address without one
   (netbsd-src/sys/netinet/in.c:1227-1235) */
static ULONG
classful_mask(ULONG a)
{

	if ((a & 0x80000000UL) == 0)
		return 0xff000000UL;
	if ((a & 0xc0000000UL) == 0x80000000UL)
		return 0xffff0000UL;
	return 0xffffff00UL;
}

static int
tokenize(char *line, char **tok, int max)
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

/* called on the interface's I/O process when the driver reports a link
   change: must not block (dhcp_wake() only signals) */
static void
link_hook(void *ctx, int up)
{
	struct iface *ifc = ctx;

	ifc->link = up;
	ifc->dhcp_event = 1;
	dhcp_wake(ifc);
	if (!ifc->dhcp)
		ifc->up = up && ifc->admin && ifc->addr_set;
	if (stack_task)
		Signal(stack_task, SIGBREAKF_CTRL_E);
}

void
stack_link_changed(void)
{
	int i;

	for (i = 0; i < nifaces; i++)
		if (ifaces[i].link != ifaces[i].link_logged) {
			ifaces[i].link_logged = ifaces[i].link;
			P("%s: link %s\n", ifaces[i].name,
			    ifaces[i].link ? "up" : "down");
		}
	stack_update_route();
}

/*
 * Plug-in adapters found at every start: PaulaNET (Wi-Fi through the
 * floppy port, https://github.com/RobSmithDev/PaulaNET).  If its driver is
 * there (DEVS:, DEVS:Networks or the adapter's own PaulaNET: disk) and no
 * configured interface uses it, it is added as an optional DHCP
 * interface, last, so a faster interface keeps the default route.  Its
 * driver opens only with the adapter plugged in; without it the
 * interface stays hidden.  "autodetect off" in the configuration turns
 * this off.  With "paulanet verify on" the driver file must be a known
 * official build or have a CRC the user accepted (src/common/drvcheck.c).
 * Also done without any configuration file.
 */
static int autodetect = 1;
static struct drvprefs drvprefs;

/* where PaulaNET.device is, looked for once per configuration */
static char paula_path[64];
static int paula_looked;

static const char *
paulanet_file(void)
{

	if (!paula_looked) {
		paula_looked = 1;
		if (!drv_paulanet_path(paula_path, sizeof(paula_path)))
			paula_path[0] = '\0';
	}
	return paula_path[0] ? paula_path : NULL;
}

#define	is_paulanet(dev)	amibsdnet_is_paulanet(dev)

static int
has_colon(const char *s)
{

	for (; *s; s++)
		if (*s == ':')
			return 1;
	return 0;
}

/* 0 if the interface must not be used: its driver failed the check */
static int
paulanet_ok(struct iface *ifc)
{
	char path[80], hex[9];		/* "DEVS:" + device[64] */
	const char *ver;
	ULONG crc;
	int r, loaded;

	ifc->unverified = 0;
	if (!drvprefs.verify || !is_paulanet(ifc->device))
		return 1;
	Forbid();
	loaded = FindName(&SysBase->DeviceList, (CONST_STRPTR)PAULANET_NAME)
	    != NULL;
	Permit();
	if (loaded) {
		/* in memory already (loaded before verifying was turned on):
		   no file is loaded now */
		P("%s: PaulaNET.device is in memory already; its file is "
		    "checked at the next start\n", ifc->name);
		return 1;
	}
	if (amibsdnet_plain_name(ifc->device)) {
		if (!paulanet_file())
			return 1;	/* nothing to check: the open fails */
		sb_copy(path, paulanet_file(), sizeof(path));
	} else if (!has_colon(ifc->device)) {
		/* a relative name, as amibsdnet_open_sana() opens it: under
		   DEVS: */
		sb_copy(path, "DEVS:", sizeof(path));
		sb_copy(path + 5, ifc->device, sizeof(path) - 5);
	} else
		sb_copy(path, ifc->device, sizeof(path));
	r = drv_check_paulanet(path, &drvprefs, &ver, &crc);
	if (r == DRV_MISSING)
		return 1;
	if (DRV_USABLE(&drvprefs, r)) {
		P("%s: %s verified (%s)\n", ifc->name, path,
		    ver ? ver : "accepted CRC");
		return 1;
	}
	drv_fmt_crc(hex, crc);
	P("%s: %s is %s (CRC %s): not used; copy it again from the "
	    "PaulaNET disk, or accept it in Settings\n", ifc->name, path,
	    r == DRV_DAMAGED ? "damaged" : "not a known build", hex);
	ifc->unverified = 1;
	return 0;
}

static void
iface_bringup(struct iface *ifc)
{
	char link[80];
	unsigned long l;

	sb_copy(link, ifc->device, sizeof(link) - 12);
	l = 0;
	while (link[l])
		l++;
	link[l++] = ':';
	{
		char tmp[12];
		int i = 0;
		ULONG u = ifc->unit;

		do {
			tmp[i++] = '0' + u % 10;
			u /= 10;
		} while (u);
		while (i)
			link[l++] = tmp[--i];
	}
	link[l] = '\0';

	if (!paulanet_ok(ifc))
		return;
	if (rump_amibsdnet_ifcreate(ifc->name, link) != 0) {
		int err = amiga_rump_errno(), again = 0;

		/* EEXIST is fine only for an interface with a working driver
		   behind it; anything else is removed and made again */
		if (err == 17 && sana_find(ifc->device, ifc->unit) == NULL) {
			rump_amibsdnet_ifflags(ifc->name, 0, NB_IFF_UP);
			rump_amibsdnet_ifdestroy(ifc->name);
			err = rump_amibsdnet_ifcreate(ifc->name, link) == 0 ? 0 :
			    amiga_rump_errno();
			/* (still there after the destroy: still no driver
			   behind it, so a failure, not "fine") */
			again = 1;
		}
		if (err != 0 && (err != 17 || again)) {
			if (ifc->optional) {
				/* a plug-in adapter that is not plugged in */
				ifc->hidden = 1;
				P("%s: %s not present (hidden; errno %d)\n",
				    ifc->name, ifc->device, err);
			} else
				P("%s: cannot attach %s (errno %d)\n", ifc->name,
				    link, err);
			return;
		}
	}
	ifc->attached = 1;
	{
		struct virtif_user *v = sana_find(ifc->device, ifc->unit);

		if (v) {
			ifc->link = sana_link(v);
			ifc->wireless = sana_is_wireless(v);
			sana_set_linkhook(v, link_hook, ifc);
		}
	}
	/* (PaulaNET joins its network by itself: set up with PaulaNET
	   Config, not WirelessManager) */
	if (ifc->wireless && !is_paulanet(ifc->device))
		wireless_start(ifc);
	if (ifc->dhcp) {
		if (dhcp_configure(ifc) != 0)
			P("%s: cannot start the DHCP client\n", ifc->name);
		return;
	}
	if (ifc->addr) {
		if (rump_amibsdnet_ifaddr4(ifc->name, ifc->addr, ifc->mask) != 0)
			P("%s: cannot set address (errno %d)\n", ifc->name,
			    amiga_rump_errno());
		else {
			ifc->gateway = cfg_gateway;
			ifc->addr_set = 1;
			ifc->up = ifc->link;
			P("%s: %lu.%lu.%lu.%lu\n", ifc->name, ifc->addr >> 24,
			    (ifc->addr >> 16) & 255, (ifc->addr >> 8) & 255,
			    ifc->addr & 255);
		}
	} else
		rump_amibsdnet_ifflags(ifc->name, NB_IFF_UP, 0);
}

static void
add_detected(void)
{
	struct iface *ifc;
	char name[16];
	int i, k;

	if (!autodetect)
		return;
	for (i = 0; i < nifaces; i++)
		if (is_paulanet(ifaces[i].device))
			return;
	if (!paulanet_file())
		return;
	if (nifaces >= MAX_IFACES) {
		P("PaulaNET driver found, but %d interfaces are configured "
		    "already\n", MAX_IFACES);
		return;
	}
	/* a free interface name */
	for (k = 0; k < 10; k++) {
		name[0] = 's'; name[1] = 'a'; name[2] = 'n'; name[3] = 'a';
		name[4] = '0' + k; name[5] = '\0';
		for (i = 0; i < nifaces && !streq(ifaces[i].name, name); i++)
			;
		if (i == nifaces)
			break;
	}
	ifc = &ifaces[nifaces];
	memset(ifc, 0, sizeof(*ifc));
	ifc->admin = 1;
	ifc->link = 1;
	ifc->link_logged = 1;
	ifc->dhcp = 1;
	ifc->optional = 1;
	ifc->mask = 0xffffff00UL;
	sb_copy(ifc->name, name, sizeof(ifc->name));
	sb_copy(ifc->device, PAULANET_NAME, sizeof(ifc->device));
	nifaces++;
	P("%s: PaulaNET driver found; used if the adapter is plugged in\n",
	    ifc->name);
}

/* ------------------------------------------------------------------------
 * reading a configuration file
 */

static void
config_defaults(struct config *cfg)
{

	memset(cfg, 0, sizeof(*cfg));
	cfg->autodetect = 1;
}

/* the words after the first n are not used */
static void
extra_words(int lineno, int n, int used)
{

	if (n > used)
		P("config line %d: extra words ignored\n", lineno);
}

/* an "interface" line; 0 if it was not taken (said why) */
static int
parse_interface(struct config *cfg, char **tok, int n, int lineno)
{
	struct iface *ifc;
	int i, mask_given = 0;

	if (n < 4) {
		P("config line %d: interface needs a name, a driver and a "
		    "unit\n", lineno);
		return 0;
	}
	if (cfg->n >= MAX_IFACES) {
		P("config line %d: more than %d interfaces; %s ignored\n",
		    lineno, MAX_IFACES, tok[1]);
		return 0;
	}
	ifc = &cfg->ifs[cfg->n];
	if (slen(tok[1]) >= sizeof(ifc->name) ||
	    slen(tok[2]) >= sizeof(ifc->device)) {
		P("config line %d: interface or driver name too long\n",
		    lineno);
		return 0;
	}
	for (i = 0; i < cfg->n; i++)
		if (streq(cfg->ifs[i].name, tok[1])) {
			P("config line %d: interface %s is configured "
			    "twice; this one is ignored\n", lineno, tok[1]);
			return 0;
		}
	memset(ifc, 0, sizeof(*ifc));
	if (!parse_ulong(tok[3], &ifc->unit)) {
		P("config line %d: bad unit \"%s\"\n", lineno, tok[3]);
		return 0;
	}
	for (i = 0; i < cfg->n; i++)
		if (streq(cfg->ifs[i].device, tok[2]) &&
		    cfg->ifs[i].unit == ifc->unit) {
			P("config line %d: %s unit %lu is used by %s "
			    "already; %s ignored\n", lineno, tok[2], ifc->unit,
			    cfg->ifs[i].name, tok[1]);
			return 0;
		}
	ifc->admin = 1;
	ifc->link = 1;
	ifc->link_logged = 1;
	sb_copy(ifc->name, tok[1], sizeof(ifc->name));
	sb_copy(ifc->device, tok[2], sizeof(ifc->device));
	for (i = 4; i < n; i++) {
		if (streq(tok[i], "dhcp"))
			ifc->dhcp = 1;
		else if (streq(tok[i], "optional"))
			ifc->optional = 1;
		/* (for the Settings window, src/tools/settingswin.c
		   iface_kind(); the stack asks the driver, sana_is_wireless()
		   in iface_bringup()) */
		else if (streq(tok[i], "wifi"))
			continue;
		else if (streq(tok[i], "address") || streq(tok[i], "netmask")) {
			int isaddr = streq(tok[i], "address");
			/* (1 is no mask: parse_ip() sets one only for a
			   "/prefix", and never 1) */
			ULONG a = 0, m = 1;

			if (i + 1 >= n) {
				P("config line %d: %s without a value\n",
				    lineno, tok[i]);
				continue;
			}
			i++;
			if (isaddr) {
				if (!parse_ip(tok[i], &a, &m)) {
					P("config line %d: bad address "
					    "\"%s\"\n", lineno, tok[i]);
					continue;
				}
				ifc->addr = a;
				if (m != 1) {
					ifc->mask = m;
					mask_given = 1;
				}
			} else if (!parse_ip(tok[i], &m, NULL) || !mask_ok(m)) {
				P("config line %d: bad netmask \"%s\"\n",
				    lineno, tok[i]);
			} else {
				ifc->mask = m;
				mask_given = 1;
			}
		} else
			P("config line %d: \"%s\" not understood\n", lineno,
			    tok[i]);
	}
	if (ifc->addr && !mask_given)
		ifc->mask = classful_mask(ifc->addr);
	if (ifc->addr && !mask_ok(ifc->mask)) {
		P("config line %d: bad prefix for %s; address not used\n",
		    lineno, ifc->name);
		ifc->addr = 0;
	}
	cfg->n++;
	return 1;
}

static void
parse_line(struct config *cfg, char *line, int lineno)
{
	char *tok[13];
	ULONG a;
	int n;

	/* (one word more than used: a 13th means the line is too long) */
	if ((n = tokenize(line, tok, 13)) == 0)
		return;
	if (n == 13) {
		P("config line %d: more than 12 words; the rest ignored\n",
		    lineno);
		n = 12;
	}
	if (streq(tok[0], "hostname") || streq(tok[0], "domain")) {
		char *dst = streq(tok[0], "hostname") ? cfg->hostname :
		    cfg->domain;

		if (n < 2)
			P("config line %d: %s without a value\n", lineno,
			    tok[0]);
		else if (slen(tok[1]) >= sizeof(cfg->hostname))
			P("config line %d: %s longer than %d characters\n",
			    lineno, tok[0], (int)sizeof(cfg->hostname) - 1);
		else {
			sb_copy(dst, tok[1], sizeof(cfg->hostname));
			extra_words(lineno, n, 2);
		}
	} else if (streq(tok[0], "nameserver") || streq(tok[0], "gateway")) {
		int ns = streq(tok[0], "nameserver");

		if (n < 2)
			P("config line %d: %s without an address\n", lineno,
			    tok[0]);
		else if (!parse_ip(tok[1], &a, NULL))
			P("config line %d: bad address \"%s\"\n", lineno,
			    tok[1]);
		else if (ns && cfg->nns >= 4)
			P("config line %d: more than 4 name servers; %s "
			    "ignored\n", lineno, tok[1]);
		else {
			if (ns)
				cfg->ns[cfg->nns++] = a;
			else
				cfg->gateway = a;
			extra_words(lineno, n, 2);
		}
	} else if (streq(tok[0], "interface")) {
		parse_interface(cfg, tok, n, lineno);
	} else if (drv_parse_line(&cfg->drv, tok, n)) {
		;
	} else if (streq(tok[0], "autodetect")) {
		if (n >= 2 && streq(tok[1], "off"))
			cfg->autodetect = 0;
		else if (n >= 2 && streq(tok[1], "on"))
			cfg->autodetect = 1;
		else
			P("config line %d: autodetect on or off\n", lineno);
	} else
		P("config line %d: not understood\n", lineno);
}

/* read a configuration file into cfg; -1 if it cannot be opened */
static int
read_config(const char *path, struct config *cfg)
{
	char line[256];
	BPTR fh;
	int lineno = 0;
	unsigned long len;

	if ((fh = Open((CONST_STRPTR)path, MODE_OLDFILE)) == 0)
		return -1;
	P("AmiBSDNet: reading %s\n", path);
	config_defaults(cfg);
	/* (FGets() gets the buffer size less one: V36 and V37 copy "one
	   more byte than it should" and put the NUL after it, and pass in
	   buffersize-1 is what the doc says for them, dos.doc:2146-2150;
	   V39 and later read at most that length less one) */
	while (FGets(fh, (STRPTR)line, sizeof(line) - 1)) {
		lineno++;
		len = slen(line);
		if (len >= sizeof(line) - 2 && line[len - 1] != '\n') {
			char rest[64];
			unsigned long rl;
			int more = 0;

			while (FGets(fh, (STRPTR)rest, sizeof(rest) - 1)) {
				more = 1;
				rl = slen(rest);
				if (rl < sizeof(rest) - 2 || rest[rl - 1] == '\n')
					break;
			}
			if (more) {
				P("config line %d: longer than %d characters; "
				    "ignored\n", lineno, (int)sizeof(line) - 3);
				continue;
			}
		}
		parse_line(cfg, line, lineno);
	}
	if (IoErr() != 0)
		P("AmiBSDNet: error %ld reading %s after line %d\n",
		    (long)IoErr(), path, lineno);
	Close(fh);
	return 0;
}

/* the configuration file (ENV:), else its saved copy (ENVARC:) */
static int
read_any(struct config *cfg)
{

	if (cfgfile[0] && read_config(cfgfile, cfg) == 0)
		return 0;
	if (cfgfallback[0] && read_config(cfgfallback, cfg) == 0)
		return 0;
	return -1;
}

/* make cfg the running configuration (no interfaces may exist) */
static void
apply_config(const struct config *cfg)
{
	int i;

	for (i = 0; i < cfg->n; i++)
		ifaces[i] = cfg->ifs[i];
	cfg_nns = cfg->nns;
	for (i = 0; i < cfg->nns; i++)
		cfg_ns[i] = cfg->ns[i];
	cfg_gateway = cfg->gateway;
	autodetect = cfg->autodetect;
	drvprefs = cfg->drv;
	paula_looked = 0;
	netdb_set_hostname(cfg->hostname[0] ? cfg->hostname :
	    DEFAULT_HOSTNAME);
	sb_copy(cfg_domain, cfg->domain, sizeof(cfg_domain));
	netdb_set_domain(cfg_domain);
	nifaces = cfg->n;
	add_detected();

	for (i = 0; i < nifaces; i++)
		iface_bringup(&ifaces[i]);
	stack_update_route();
	stack_update_dns();
}

/* the configuration being read and the one waiting to be applied */
static struct config readcfg, pendingcfg;
static int reconfig_active;

/*
 * First configuration at startup.  Both paths are kept, so a later
 * reconfiguration reads the file the settings window writes even if none
 * existed at boot.  Without a file, detected adapters are still set up.
 * Returns -1 if no file could be read.
 */
int
stack_configure_from(const char *path, const char *fallback)
{
	int r;

	if (!routesem_ready) {
		InitSemaphore(&routesem);
		routesem_ready = 1;
	}
	stack_task = SysBase->ThisTask;
	sb_copy(cfgfile, path, sizeof(cfgfile));
	sb_copy(cfgfallback, fallback ? fallback : "", sizeof(cfgfallback));
	if ((r = read_any(&readcfg)) != 0) {
		P("AmiBSDNet: no configuration file (%s, %s)\n", cfgfile,
		    cfgfallback[0] ? cfgfallback : "-");
		config_defaults(&readcfg);
	}
	config_generation++;
	apply_config(&readcfg);
	return r;
}

/* the old interfaces go (their DHCP clients have ended), the new
   configuration comes */
void
stack_reconfigure_poll(void)
{
	int i;

	if (!reconfig_active || dhcp_clients > 0)
		return;
	/* (not while an interface call of the library works on the table,
	   src/lib/ifapi.c; one of them may have started a client) */
	ObtainSemaphore(&iftable_lock);
	if (dhcp_clients > 0) {
		ReleaseSemaphore(&iftable_lock);
		return;
	}
	for (i = 0; i < nifaces; i++) {
		ifaces[i].up = 0;
		ifaces[i].gateway = 0;
	}
	stack_update_route();
	for (i = 0; i < nifaces; i++) {
		struct iface *ifc = &ifaces[i];

		if (!ifc->attached)	/* its driver never opened */
			continue;
		P("%s: removing\n", ifc->name);
		if (ifc->addr)
			rump_amibsdnet_ifdeladdr4(ifc->name, ifc->addr);
		/* virtif refuses to remove an interface that is up */
		rump_amibsdnet_ifflags(ifc->name, 0, NB_IFF_UP);
		if (rump_amibsdnet_ifdestroy(ifc->name) != 0)
			P("%s: cannot remove (errno %d)\n", ifc->name,
			    amiga_rump_errno());
	}
	nifaces = 0;
	stack_update_route();
	apply_config(&pendingcfg);
	reconfig_active = 0;
	ReleaseSemaphore(&iftable_lock);
	control_reconfig_done(0);
}

/* a reconfiguration is waiting for the DHCP clients to end */
int
stack_reconfig_pending(void)
{

	return reconfig_active;
}

/*
 * Apply a changed configuration file (from the settings window or
 * NetCtrl RECONFIG).  The file is read first: if it cannot be read, the
 * running configuration stays.  Then the DHCP clients are told to stop;
 * once the last has ended (it signals the stack process,
 * stack_reconfigure_poll()) the interfaces are removed, so their drivers
 * are closed and may change, and everything is set up again.  Meanwhile
 * the stack process goes on serving the control port.
 */
int
stack_reconfigure_begin(void)
{
	int i;

	if (read_any(&readcfg) != 0) {
		P("AmiBSDNet: no configuration file could be read; the "
		    "configuration in use stays\n");
		return -1;
	}
	pendingcfg = readcfg;
	P("AmiBSDNet: applying the new configuration\n");
	if (!reconfig_active) {
		reconfig_active = 1;
		config_generation++;
		for (i = 0; i < nifaces; i++)
			dhcp_wake(&ifaces[i]);
	}
	stack_reconfigure_poll();
	return reconfig_active ? 1 : 0;
}

void
stack_offline(void)
{
	int i, client;

	for (i = 0; i < nifaces; i++) {
		struct iface *ifc = &ifaces[i];

		ifc->admin = 0;
		ifc->release = 1;
		ifc->dhcp_event = 1;
		Forbid();
		client = ifc->dhcp_task != NULL;
		Permit();
		if (client) {
			/* the DHCP client releases the lease first, then
			   takes the interface down itself */
			dhcp_wake(ifc);
			continue;
		}
		rump_amibsdnet_ifflags(ifc->name, 0, NB_IFF_UP);
		ifc->up = 0;
	}
	stack_update_route();
}

void
stack_online(void)
{
	int i;

	for (i = 0; i < nifaces; i++) {
		struct iface *ifc = &ifaces[i];

		ifc->admin = 1;
		ifc->dhcp_event = 1;	/* the DHCP client takes it from here */
		dhcp_wake(ifc);
		rump_amibsdnet_ifflags(ifc->name, NB_IFF_UP, 0);
		if (!ifc->dhcp && ifc->addr_set)
			ifc->up = ifc->link;
	}
	stack_update_route();
}

/*
 * Is the network really working (for the trial switch)?  An address is
 * not enough: with DHCP a server must have answered (a lease), with a
 * fixed address frames must have arrived from the network.
 */
int
stack_connected(void)
{
	int i;

	for (i = 0; i < nifaces; i++) {
		struct iface *ifc = &ifaces[i];
		struct virtif_user *v;
		ULONG rx = 0, tx, rxd, txd;

		if (!ifc->up || !ifc->addr)
			continue;
		if (ifc->dhcp)
			return 1;
		if ((v = sana_find(ifc->device, ifc->unit)) != NULL) {
			sana_stats(v, &rx, &tx, &rxd, &txd);
			if (rx > 0)
				return 1;
		}
	}
	return 0;
}

ULONG
netcfg_primary_address(void)
{
	int i;

	for (i = 0; i < nifaces; i++)
		if (ifaces[i].up && ifaces[i].addr)
			return ifaces[i].addr;
	return 0x7f000001UL;
}

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

extern struct ExecBase *SysBase;

struct iface ifaces[MAX_IFACES];
int nifaces;

static ULONG cfg_ns[4];
static int cfg_nns;
static ULONG cfg_gateway;
static char cfgfile[256], cfgfallback[256];	/* ENV: and ENVARC: copies */

/* bumped on every (re)configuration; DHCP renewers of an older
   generation stop */
volatile ULONG config_generation;

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

void
stack_update_route(void)
{
	ULONG gw = 0, ifa = 0;
	int i;

	ObtainSemaphore(&routesem);
	for (i = 0; i < nifaces; i++)
		if (ifaces[i].up && ifaces[i].gateway) {
			gw = ifaces[i].gateway;
			ifa = ifaces[i].addr;
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
				P("default route via %lu.%lu.%lu.%lu\n", gw >> 24,
				    (gw >> 16) & 255, (gw >> 8) & 255, gw & 255);
			} else
				P("default route: errno %d\n", amiga_rump_errno());
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
	while (*s >= '0' && *s <= '9')
		n = n * 10 + (*s++ - '0');
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
	int i;

	for (i = 0; i < 4; i++) {
		if (*s < '0' || *s > '9')
			return 0;
		for (part = 0; *s >= '0' && *s <= '9'; s++)
			part = part * 10 + (*s - '0');
		if (part > 255)
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
   change: must not block */
static struct Task *stacktask;

static void
link_hook(void *ctx, int up)
{
	struct iface *ifc = ctx;

	ifc->link = up;
	ifc->dhcp_event = 1;
	if (!ifc->dhcp)
		ifc->up = up && ifc->admin && ifc->addr_set;
	if (stacktask)
		Signal(stacktask, SIGBREAKF_CTRL_E);
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
		/* relative names are relative to DEVS: for OpenDevice() */
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
		int err = amiga_rump_errno();

		/* EEXIST is fine only for an interface with a working driver
		   behind it; anything else is removed and made again */
		if (err == 17 && sana_find(ifc->device, ifc->unit) == NULL) {
			rump_amibsdnet_ifflags(ifc->name, 0, NB_IFF_UP);
			rump_amibsdnet_ifdestroy(ifc->name);
			err = rump_amibsdnet_ifcreate(ifc->name, link) == 0 ? 0 :
			    amiga_rump_errno();
		}
		if (err != 0 && err != 17) {
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

int
stack_configure(const char *path)
{
	char line[256], *tok[12];
	BPTR fh;
	int n, i, lineno = 0;

	if (!routesem_ready) {
		InitSemaphore(&routesem);
		routesem_ready = 1;
	}
	if ((fh = Open((CONST_STRPTR)path, MODE_OLDFILE)) == 0)
		return -1;
	stacktask = SysBase->ThisTask;
	P("AmiBSDNet: reading %s\n", path);
	config_generation++;
	nifaces = 0;
	cfg_nns = 0;
	cfg_gateway = 0;
	autodetect = 1;
	drvprefs.verify = 0;
	drvprefs.ncrc = 0;
	paula_looked = 0;
	while (FGets(fh, (STRPTR)line, sizeof(line))) {
		lineno++;
		if ((n = tokenize(line, tok, 12)) == 0)
			continue;
		if (streq(tok[0], "hostname") && n >= 2) {
			netdb_set_hostname(tok[1]);
		} else if (streq(tok[0], "domain") && n >= 2) {
			netdb_set_domain(tok[1]);
		} else if (streq(tok[0], "nameserver") && n >= 2) {
			if (cfg_nns < 4 && parse_ip(tok[1], &cfg_ns[cfg_nns], NULL))
				cfg_nns++;
		} else if (streq(tok[0], "gateway") && n >= 2) {
			parse_ip(tok[1], &cfg_gateway, NULL);
		} else if (streq(tok[0], "interface") && n >= 4 &&
		    nifaces < MAX_IFACES) {
			struct iface *ifc = &ifaces[nifaces];

			memset(ifc, 0, sizeof(*ifc));
			ifc->admin = 1;
			ifc->link = 1;
			ifc->link_logged = 1;
			sb_copy(ifc->name, tok[1], sizeof(ifc->name));
			sb_copy(ifc->device, tok[2], sizeof(ifc->device));
			if (!parse_ulong(tok[3], &ifc->unit)) {
				P("config line %d: bad unit\n", lineno);
				continue;
			}
			ifc->mask = 0xffffff00UL;
			for (i = 4; i < n; i++) {
				if (streq(tok[i], "dhcp"))
					ifc->dhcp = 1;
				else if (streq(tok[i], "address") && i + 1 < n)
					parse_ip(tok[++i], &ifc->addr, &ifc->mask);
				else if (streq(tok[i], "netmask") && i + 1 < n)
					parse_ip(tok[++i], &ifc->mask, NULL);
				else if (streq(tok[i], "optional"))
					ifc->optional = 1;
			}
			nifaces++;
		} else if (drv_parse_line(&drvprefs, tok, n)) {
			;
		} else if (streq(tok[0], "autodetect") && n >= 2) {
			autodetect = !streq(tok[1], "off");
		} else
			P("config line %d: not understood\n", lineno);
	}
	Close(fh);
	add_detected();

	for (i = 0; i < nifaces; i++)
		iface_bringup(&ifaces[i]);
	stack_update_route();
	stack_update_dns();
	return 0;
}

void
stack_offline(void)
{
	int i;

	for (i = 0; i < nifaces; i++) {
		ifaces[i].admin = 0;
		ifaces[i].release = 1;
		ifaces[i].dhcp_event = 1;
		rump_amibsdnet_ifflags(ifaces[i].name, 0, NB_IFF_UP);
		ifaces[i].up = 0;
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
		rump_amibsdnet_ifflags(ifc->name, NB_IFF_UP, 0);
		if (!ifc->dhcp && ifc->addr_set)
			ifc->up = ifc->link;
	}
	stack_update_route();
}

/* the configuration file (ENV:), else its saved copy (ENVARC:) */
static int
configure_any(void)
{

	if (stack_configure(cfgfile) == 0)
		return 0;
	if (cfgfallback[0] && stack_configure(cfgfallback) == 0)
		return 0;
	return -1;
}

/*
 * First configuration at startup.  Both paths are kept, so a later
 * reconfiguration reads the file the settings window writes even if none
 * existed at boot.
 */
int
stack_configure_from(const char *path, const char *fallback)
{

	sb_copy(cfgfile, path, sizeof(cfgfile));
	sb_copy(cfgfallback, fallback ? fallback : "", sizeof(cfgfallback));
	return configure_any();
}

/*
 * Apply a changed configuration file (from the settings window or
 * NetCtrl RECONFIG): stop the DHCP clients and remove the interfaces, so
 * their drivers are closed and may change, then set everything up again.
 */
int
stack_reconfigure(void)
{
	int i, waited;

	if (!cfgfile[0])
		return -1;
	P("AmiBSDNet: applying the new configuration\n");
	config_generation++;
	for (waited = 0; dhcp_clients > 0 && waited < 200; waited++)
		Delay(5);		/* they stop within a second */
	if (dhcp_clients > 0) {
		/* removing interfaces under a running client would leave it
		   with freed memory: refuse, the settings window says so */
		P("AmiBSDNet: DHCP clients still running; configuration not "
		    "applied, try again\n");
		return -1;
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
	return configure_any();
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

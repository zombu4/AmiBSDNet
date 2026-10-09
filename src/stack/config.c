/*
 * AmiBSDNet: configuration file parsing and interface setup.
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "rumpuser_amiga.h"
#include "stack.h"

#define	P	amiga_rump_printf

struct iface ifaces[MAX_IFACES];
int nifaces;

static ULONG cfg_ns[4];
static int cfg_nns;
static ULONG cfg_gateway;
static char cfgfile[256];

/* bumped on every (re)configuration; DHCP renewers of an older
   generation stop */
volatile ULONG config_generation;

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
	*addr = v;
	if (*s == '/' && mask) {
		ULONG plen;

		if (!parse_ulong(s + 1, &plen) || plen > 32)
			return 0;
		*mask = plen ? 0xffffffffUL << (32 - plen) : 0;
		return 1;
	}
	return *s == '\0';
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

	if (rump_amibsdnet_ifcreate(ifc->name, link) != 0 &&
	    amiga_rump_errno() != 17 /* EEXIST: reconfiguring */) {
		P("%s: cannot attach %s (errno %d)\n", ifc->name, link,
		    amiga_rump_errno());
		return;
	}
	if (ifc->dhcp) {
		if (dhcp_configure(ifc) != 0)
			P("%s: no DHCP lease\n", ifc->name);
		return;
	}
	if (ifc->addr) {
		if (rump_amibsdnet_ifaddr4(ifc->name, ifc->addr, ifc->mask) != 0)
			P("%s: cannot set address (errno %d)\n", ifc->name,
			    amiga_rump_errno());
		else {
			ifc->up = 1;
			P("%s: %lu.%lu.%lu.%lu\n", ifc->name, ifc->addr >> 24,
			    (ifc->addr >> 16) & 255, (ifc->addr >> 8) & 255,
			    ifc->addr & 255);
		}
	} else
		rump_amibsdnet_ifflags(ifc->name, NB_IFF_UP, 0);
}

int
stack_configure(const char *path)
{
	char line[256], *tok[12];
	BPTR fh;
	int n, i, lineno = 0;

	if ((fh = Open((CONST_STRPTR)path, MODE_OLDFILE)) == 0)
		return -1;
	sb_copy(cfgfile, path, sizeof(cfgfile));
	P("AmiBSDNet: reading %s\n", path);
	config_generation++;
	nifaces = 0;
	cfg_nns = 0;
	cfg_gateway = 0;
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
			}
			nifaces++;
		} else
			P("config line %d: not understood\n", lineno);
	}
	Close(fh);

	for (i = 0; i < nifaces; i++)
		iface_bringup(&ifaces[i]);
	if (cfg_gateway) {
		if (rump_amibsdnet_route4(NB_RTM_ADD, 0, 0, cfg_gateway) != 0)
			P("default route: errno %d\n", amiga_rump_errno());
		else
			P("default route via %lu.%lu.%lu.%lu\n", cfg_gateway >> 24,
			    (cfg_gateway >> 16) & 255, (cfg_gateway >> 8) & 255,
			    cfg_gateway & 255);
	}
	if (cfg_nns)
		netdb_set_nameservers(cfg_ns, cfg_nns);
	return 0;
}

void
stack_offline(void)
{
	int i;

	for (i = 0; i < nifaces; i++) {
		rump_amibsdnet_ifflags(ifaces[i].name, 0, NB_IFF_UP);
		ifaces[i].up = 0;
	}
}

void
stack_online(void)
{
	int i;

	for (i = 0; i < nifaces; i++) {
		struct iface *ifc = &ifaces[i];

		rump_amibsdnet_ifflags(ifc->name, NB_IFF_UP, 0);
		if (ifc->dhcp && !ifc->addr)
			dhcp_configure(ifc);
		else if (ifc->addr)
			ifc->up = 1;
	}
}

/* drop all addresses and the default route, then read the file again */
int
stack_reconfigure(void)
{
	int i;

	if (!cfgfile[0])
		return -1;
	for (i = 0; i < nifaces; i++) {
		struct iface *ifc = &ifaces[i];

		if (ifc->addr)
			rump_amibsdnet_ifdeladdr4(ifc->name, ifc->addr);
		if (ifc->gateway)
			rump_amibsdnet_route4(NB_RTM_DELETE, 0, 0, ifc->gateway);
		ifc->up = 0;
	}
	if (cfg_gateway)
		rump_amibsdnet_route4(NB_RTM_DELETE, 0, 0, cfg_gateway);
	return stack_configure(cfgfile);
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

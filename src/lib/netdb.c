/*
 * bsdsocket.library: address conversion, name resolution and the network
 * databases.
 *
 * "the doc" is downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc,
 * "netdb.h" is .../netinclude/netdb.h, "the SDK header" is
 * .../netinclude/libraries/bsdsocket.h, "the catalog" .../locale/bsdsocket.cd.
 *
 * Host names are resolved from the hosts file (DEVS:Internet/hosts, the
 * file the Roadshow installer of this project keeps: src/tools/roadshow.c),
 * then by DNS over UDP (over TCP when the answer is truncated) to the
 * configured name servers.  Everything that needs files or sockets runs
 * on the base's server thread (a DOS process), so it is interruptible
 * with the break signal like any other blocking call.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "sblib.h"

#define	HOSTS_FILE	"DEVS:Internet/hosts"
/* the services file of the Roadshow configuration directory (named in
   the FILES section of rsh in the SDK's source_code/Roadshow/rsh.c) */
#define	SERVICES_FILE	"DEVS:Internet/services"
#define	MAXNS		8	/* name servers asked, in all */
#define	MAXSTATICNS	4	/* configured by the stack (netdb_set_nameservers) */
#define	DNS_PORT	53
#define	DNS_TIMEOUT_S	5	/* wait for one answer */
#define	DNS_TRIES	3
#define	DNS_REPLY_MAX	2048

/* resolver tracing in the stack log with DEBUG */
#define	DNSDBG(...)	do { if (amiga_rump_debug) amiga_rump_printf(__VA_ARGS__); } while (0)

/* netdb.h */
#define	AI_PASSIVE	1
#define	AI_CANONNAME	2
#define	AI_NUMERICHOST	4
#define	AI_NUMERICSERV	16
#define	AI_MASK		(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | \
			    AI_NUMERICSERV)
#define	NI_NUMERICHOST	1
#define	NI_NUMERICSERV	2
#define	NI_NOFQDN	4
#define	NI_NAMEREQD	8
#define	NI_DGRAM	16
#define	NI_WITHSCOPEID	32
#define	NI_ALL		(NI_NUMERICHOST | NI_NUMERICSERV | NI_NOFQDN | \
			    NI_NAMEREQD | NI_DGRAM | NI_WITHSCOPEID)
#define	EAI_BADFLAGS	(-1)
#define	EAI_NONAME	(-2)
#define	EAI_AGAIN	(-3)
#define	EAI_FAIL	(-4)
#define	EAI_NODATA	(-5)
#define	EAI_FAMILY	(-6)
#define	EAI_SOCKTYPE	(-7)
#define	EAI_SERVICE	(-8)
#define	EAI_ADDRFAMILY	(-9)
#define	EAI_MEMORY	(-10)
#define	EAI_SYSTEM	(-11)
#define	EAI_BADHINTS	(-12)
#define	EAI_PROTOCOL	(-13)

/* the SDK header: struct DomainNameServerNode */
struct DomainNameServerNode {
	struct MinNode dnsn_MinNode;
	LONG dnsn_Size;
	STRPTR dnsn_Address;
	LONG dnsn_UseCount;
};

/* name servers: those the stack configured (statically: use count -1 in
   the DNS API) and those added with AddDomainNameServer() */
static ULONG static_ns[MAXSTATICNS];
static int nstatic;
struct dnsadd {
	struct dnsadd *next;
	ULONG addr;
	LONG count;
};
static struct dnsadd *dnsadded;
static char domainname[256];
static char hostname_cfg[128] = "amiga";
static struct SignalSemaphore nslock;
static int nslock_init;

void
netdb_init(void)
{

	Forbid();
	if (!nslock_init) {
		InitSemaphore(&nslock);
		nslock_init = 1;
	}
	Permit();
}

static void
ns_lock(void)
{

	netdb_init();
	ObtainSemaphore(&nslock);
}

void
netdb_set_nameservers(const ULONG *addrs, int n)
{
	int i;

	ns_lock();
	for (i = 0; i < n && i < MAXSTATICNS; i++)
		static_ns[i] = addrs[i];
	nstatic = i;
	ReleaseSemaphore(&nslock);
}

/* the servers the resolver asks: the in-memory database first ("The
   local in-memory domain name server database is scanned before the
   on-disk configuration file is examined", the doc,
   AddDomainNameServer), then the configured ones; each once */
int
netdb_get_nameservers(ULONG *addrs, int max)
{
	struct dnsadd *d;
	int i, k, n = 0;

	ns_lock();
	for (d = dnsadded; d && n < max; d = d->next)
		addrs[n++] = d->addr;
	for (i = 0; i < nstatic && n < max; i++) {
		for (k = 0; k < n && addrs[k] != static_ns[i]; k++)
			;
		if (k == n)
			addrs[n++] = static_ns[i];
	}
	ReleaseSemaphore(&nslock);
	return n;
}

int
netdb_nameserver_count(void)
{
	ULONG all[MAXNS];

	return netdb_get_nameservers(all, MAXNS);
}

void
netdb_set_domain(const char *d)
{

	ns_lock();
	sb_strlcpy(domainname, d ? d : "", sizeof(domainname));
	ReleaseSemaphore(&nslock);
}

const char *
netdb_get_domain(void)
{

	return domainname;
}

void
netdb_set_hostname(const char *h)
{

	sb_strlcpy(hostname_cfg, h, sizeof(hostname_cfg));
}

/* ------------------------------------------------------------------------
 * address conversion (pure: run on the caller's stack)
 */

static int
isdigit_(int c)
{

	return c >= '0' && c <= '9';
}

static int
isspace_(int c)
{

	return c == ' ' || (c >= '\t' && c <= '\r');
}

static int
hexval(int c)
{

	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/*
 * One number of the dotted notation: "decimal, octal, or hexadecimal, as
 * specified in the C language (i.e., a leading 0x or 0X implies
 * hexadecimal; otherwise, a leading 0 implies octal; otherwise, the number
 * is interpreted as decimal)" (the doc, inet_addr).  Returns the end of
 * the number, or NULL if there is none or it does not fit 32 bits.
 */
static const char *
parse_num(const char *cp, ULONG *res)
{
	ULONG v = 0;
	int base = 10, d, nd = 0;

	if (*cp == '0') {
		if (cp[1] == 'x' || cp[1] == 'X') {
			base = 16;
			cp += 2;
		} else
			base = 8;
	}
	while ((d = hexval(*cp)) >= 0 && (base == 16 || isdigit_(*cp))) {
		if (d >= base || v > (0xffffffffUL - d) / base)
			return NULL;
		v = v * base + d;
		cp++;
		nd++;
	}
	if (nd == 0)
		return NULL;
	*res = v;
	return cp;
}

/* inet_aton(): a.b.c.d, a.b.c, a.b, a (the doc, inet_addr) */
static int
parse_inet_aton(const char *cp, ULONG *res)
{
	ULONG parts[3], val;
	int n = 0;

	for (;;) {
		if ((cp = parse_num(cp, &val)) == NULL)
			return 0;
		if (*cp == '.') {
			if (n >= 3 || val > 0xff)
				return 0;
			parts[n++] = val;
			cp++;
			continue;
		}
		break;
	}
	if (*cp && !isspace_((UBYTE)*cp))
		return 0;
	switch (n) {
	case 0:
		break;
	case 1:
		if (val > 0xffffff) return 0;
		val |= parts[0] << 24;
		break;
	case 2:
		if (val > 0xffff) return 0;
		val |= (parts[0] << 24) | (parts[1] << 16);
		break;
	case 3:
		if (val > 0xff) return 0;
		val |= (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8);
		break;
	}
	*res = val;
	return 1;
}

int
netdb_parse_inet_aton(const char *cp, ULONG *res)
{

	return parse_inet_aton(cp, res);
}

/* inet_network(): up to four parts, each one byte, first the most
   significant; INADDR_NONE for a malformed string */
ULONG
netdb_inet_network(const char *p)
{
	ULONG v, net = 0;
	int n = 0;

	for (;;) {
		if ((p = parse_num(p, &v)) == NULL || v > 255 || n == 4)
			return INADDR_NONE;
		net = (net << 8) | v;
		n++;
		if (*p != '.')
			break;
		p++;
	}
	return *p && !isspace_((UBYTE)*p) ? INADDR_NONE : net;
}

/* strict a.b.c.d, decimal parts 0..255 (the doc, inet_pton) */
static int
parse_dotted4(const char *s, ULONG *res)
{
	ULONG v = 0, part;
	int n;

	for (n = 0; n < 4; n++) {
		if (!isdigit_(*s))
			return 0;
		part = 0;
		while (isdigit_(*s)) {
			part = part * 10 + (*s++ - '0');
			if (part > 255)
				return 0;
		}
		v = (v << 8) | part;
		if (n < 3 && *s++ != '.')
			return 0;
	}
	if (*s)
		return 0;
	*res = v;
	return 1;
}

static void
fmt_ip4(char *buf, ULONG ip)
{
	char *p = buf;

	p = sb_fmt_ulong(p, ip >> 24); *p++ = '.';
	p = sb_fmt_ulong(p, (ip >> 16) & 0xff); *p++ = '.';
	p = sb_fmt_ulong(p, (ip >> 8) & 0xff); *p++ = '.';
	p = sb_fmt_ulong(p, ip & 0xff);
	*p = '\0';
}

STRPTR
sb_Inet_NtoA(struct SocketBase *sb, LONG ip)
{

	fmt_ip4(sb->ntoabuf, (ULONG)ip);
	return (STRPTR)sb->ntoabuf;
}

in_addr_t
sb_inet_addr(struct SocketBase *sb, STRPTR cp)
{
	ULONG v;

	return cp != NULL && parse_inet_aton((const char *)cp, &v) ? v :
	    INADDR_NONE;
}

LONG
sb_inet_aton(struct SocketBase *sb, STRPTR cp, struct in_addr *addr)
{
	ULONG v;

	if (cp == NULL || !parse_inet_aton((const char *)cp, &v))
		return 0;
	if (addr)
		addr->s_addr = v;
	return 1;
}

/* classful helpers, kept for old software */
#define	IN_CLASSA(i)	(((ULONG)(i) & 0x80000000UL) == 0)
#define	IN_CLASSB(i)	(((ULONG)(i) & 0xc0000000UL) == 0x80000000UL)

in_addr_t
sb_Inet_LnaOf(struct SocketBase *sb, LONG in)
{
	ULONG i = in;

	if (IN_CLASSA(i)) return i & 0x00ffffff;
	if (IN_CLASSB(i)) return i & 0x0000ffff;
	return i & 0x000000ff;
}

in_addr_t
sb_Inet_NetOf(struct SocketBase *sb, LONG in)
{
	ULONG i = in;

	if (IN_CLASSA(i)) return (i & 0xff000000) >> 24;
	if (IN_CLASSB(i)) return (i & 0xffff0000) >> 16;
	return (i & 0xffffff00) >> 8;
}

in_addr_t
sb_Inet_MakeAddr(struct SocketBase *sb, LONG net, LONG host)
{
	ULONG n = net, h = host;

	if (n < 128) return (n << 24) | (h & 0x00ffffff);
	if (n < 65536) return (n << 16) | (h & 0x0000ffff);
	if (n < 16777216) return (n << 8) | (h & 0x000000ff);
	return n | h;
}

in_addr_t
sb_inet_network(struct SocketBase *sb, STRPTR cp)
{

	return cp == NULL ? INADDR_NONE : netdb_inet_network((const char *)cp);
}

/* inet_ntop / inet_pton for AF_INET and AF_INET6 */
static const char hexd[] = "0123456789abcdef";

/* inet_ntop() without the errno: 0 or the error code (also for the
   server thread, which must not set errno: the error hook runs in the
   caller's context, library.c sb_rpc) */
static LONG
ntop(LONG af, const UBYTE *a, STRPTR dst, LONG size)
{
	char buf[48], *p = buf;
	int i, best = -1, bestlen = 0, cur = -1, curlen = 0;
	UWORD w[8];

	if (a == NULL || dst == NULL)
		return EFAULT;
	if (af == AF_INET) {
		fmt_ip4(buf, ((ULONG)a[0] << 24) | (a[1] << 16) | (a[2] << 8) |
		    a[3]);
	} else if (af == AF_INET6) {
		for (i = 0; i < 8; i++)
			w[i] = (a[2 * i] << 8) | a[2 * i + 1];
		for (i = 0; i < 8; i++) {	/* longest run of zeros */
			if (w[i] == 0) {
				if (cur < 0)
					cur = i, curlen = 0;
				if (++curlen > bestlen)
					best = cur, bestlen = curlen;
			} else
				cur = -1;
		}
		if (bestlen < 2)
			best = -1;
		for (i = 0; i < 8; i++) {
			if (i == best) {
				/* one ':' here, the next group adds its own
				   (and one more below if the run is at the end):
				   "::1", "1::2", "1::", "::" */
				*p++ = ':';
				i += bestlen - 1;
				continue;
			}
			if (i)
				*p++ = ':';
			{
				int sh, started = 0;

				for (sh = 12; sh >= 0; sh -= 4) {
					int d = (w[i] >> sh) & 15;

					if (d || started || sh == 0)
						*p++ = hexd[d], started = 1;
				}
			}
		}
		if (best >= 0 && best + bestlen == 8)
			*p++ = ':';
		*p = '\0';
	} else
		return EAFNOSUPPORT;
	if ((LONG)sb_strlen(buf) + 1 > size)
		return ENOSPC;
	sb_strlcpy((char *)dst, buf, size);
	return 0;
}

STRPTR
sb_inet_ntop(struct SocketBase *sb, LONG af, APTR src, STRPTR dst, LONG size)
{
	LONG e = ntop(af, src, dst, size);

	if (e) {
		sb_set_errno(sb, e);
		return NULL;
	}
	return dst;
}

static int
pton6(const char *s, UBYTE *out)
{
	UWORD w[8];
	int n = 0, gap = -1, i, v, d, digits;

	if (*s == ':' && *++s != ':')
		return 0;
	while (*s && n < 8) {
		if (*s == ':') {
			if (gap >= 0)
				return 0;
			gap = n;
			s++;
			continue;
		}
		/* embedded IPv4 in the last 32 bits */
		{
			const char *q = s;
			ULONG v4;

			while (*q && *q != ':' && *q != '.')
				q++;
			if (*q == '.' && n <= 6 && parse_dotted4(s, &v4)) {
				w[n++] = v4 >> 16;
				w[n++] = v4 & 0xffff;
				s += sb_strlen(s);
				break;
			}
		}
		v = 0;
		digits = 0;
		while ((d = hexval(*s)) >= 0 && digits < 4)
			v = (v << 4) | d, s++, digits++;
		if (!digits)
			return 0;
		w[n++] = v;
		if (*s == ':') {
			s++;
			if (*s == ':') {
				if (gap >= 0)
					return 0;
				gap = n;
				s++;
			} else if (!*s)
				return 0;
		} else if (*s)
			return 0;
	}
	if (*s)
		return 0;
	if (gap >= 0) {
		int move = n - gap;

		/* "::" stands for one group or more: with eight groups
		   besides it the address is refused ("if (tp == ns_in6addrsz)
		   return 0;", downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/
		   source_code/wget-1.10.1/src/host.c:582-585) */
		if (n == 8)
			return 0;

		for (i = 7; i >= 8 - move; i--)
			w[i] = w[gap + move - (8 - i)];
		for (i = gap; i < 8 - move; i++)
			w[i] = 0;
	} else if (n != 8)
		return 0;
	for (i = 0; i < 8; i++)
		out[2 * i] = w[i] >> 8, out[2 * i + 1] = w[i] & 0xff;
	return 1;
}

LONG
sb_inet_pton(struct SocketBase *sb, LONG af, STRPTR src, APTR dst)
{
	ULONG v;
	UBYTE t6[16];

	if (src == NULL || dst == NULL) {
		sb_set_errno(sb, EFAULT);
		return -1;
	}
	if (af == AF_INET) {
		UBYTE *d = dst;

		if (!parse_dotted4((const char *)src, &v))
			return 0;
		d[0] = v >> 24; d[1] = v >> 16; d[2] = v >> 8; d[3] = v;
		return 1;
	}
	if (af == AF_INET6) {
		if (!pton6((const char *)src, t6))
			return 0;
		CopyMem(t6, dst, 16);
		return 1;
	}
	sb_set_errno(sb, EAFNOSUPPORT);
	return -1;
}

/* ------------------------------------------------------------------------
 * text databases (hosts, services)
 */

/*
 * The next line of a text file, split into whitespace separated tokens
 * ('#' starts a comment); the number of tokens (0 for a blank line), or -1
 * at the end of the file.  The tokens are in 'line'.  What does not fit
 * into 'line' is dropped, and is not taken for a line of its own.
 */
static int
text_line(BPTR fh, char *line, int size, char **tok, int maxtok)
{
	char *p;
	int nt = 0, len, c;

	/* (the size less one for dos V36/V37, which copy one byte more:
	   dos.doc:2146-2150, FGets BUGS) */
	if (FGets(fh, (STRPTR)line, size - 1) == NULL)
		return -1;
	len = sb_strlen(line);
	if (len >= size - 2 && line[len - 1] != '\n')
		while ((c = FGetC(fh)) != -1 && c != '\n')
			;
	p = line;
	while (nt < maxtok) {
		while (isspace_((UBYTE)*p))
			p++;
		if (*p == '\0' || *p == '#')
			break;
		tok[nt++] = p;
		while (*p && !isspace_((UBYTE)*p) && *p != '#')
			p++;
		if (*p == '\0' || *p == '#') {
			*p = '\0';
			break;
		}
		*p++ = '\0';
	}
	return nt;
}

/* ------------------------------------------------------------------------
 * results of name resolution
 */

struct dns_result {
	char cname[256];	/* canonical name */
	int naddr;
	UBYTE addr[8][16];
	char ptrname[256];
	int nalias;		/* other names of the host */
	char aliasbuf[384];	/* NUL separated */
	int servfail;		/* a server answered SERVFAIL (dns_query()) */
};

static void
res_clear(struct dns_result *res)
{

	memset(res, 0, sizeof(*res));
}

/* remember another name of the host (the same name only once) */
static void
res_alias(struct dns_result *res, const char *name)
{
	const char *a = res->aliasbuf;
	size_t used = 0, l = sb_strlen(name);
	int i;

	if (sb_strcasecmp(name, res->cname) == 0)
		return;
	for (i = 0; i < res->nalias; i++) {
		if (sb_strcasecmp(a, name) == 0)
			return;
		a += sb_strlen(a) + 1;
		used = a - res->aliasbuf;
	}
	if (res->nalias >= 8 || used + l + 1 > sizeof(res->aliasbuf))
		return;
	sb_strlcpy(res->aliasbuf + used, name, l + 1);
	res->nalias++;
}

/* ------------------------------------------------------------------------
 * hosts file
 */

/* look name up in the hosts file; fills canonical name, aliases and the
   addresses; returns the number of addresses found */
static int
hosts_lookup_name(const char *name, struct dns_result *res)
{
	char line[256], *tok[16];
	BPTR fh;
	int nt, i, found = 0;
	ULONG a;

	if ((fh = Open((CONST_STRPTR)HOSTS_FILE, MODE_OLDFILE)) == 0)
		return 0;
	while ((nt = text_line(fh, line, sizeof(line), tok, 16)) >= 0) {
		if (nt < 2 || !parse_inet_aton(tok[0], &a))
			continue;
		for (i = 1; i < nt; i++)
			if (sb_strcasecmp(tok[i], name) == 0)
				break;
		if (i == nt)
			continue;
		if (found == 0) {
			sb_strlcpy(res->cname, tok[1], sizeof(res->cname));
			for (i = 2; i < nt; i++)
				res_alias(res, tok[i]);
		}
		if (res->naddr < 8) {
			CopyMem(&a, res->addr[res->naddr++], 4);
			found++;
		}
	}
	Close(fh);
	return found;
}

/* the name of an address in the hosts file; 1 if found */
static int
hosts_lookup_addr(ULONG addr, struct dns_result *res)
{
	char line[256], *tok[16];
	BPTR fh;
	int nt, i;
	ULONG a;

	if ((fh = Open((CONST_STRPTR)HOSTS_FILE, MODE_OLDFILE)) == 0)
		return 0;
	while ((nt = text_line(fh, line, sizeof(line), tok, 16)) >= 0) {
		if (nt < 2 || !parse_inet_aton(tok[0], &a) || a != addr)
			continue;
		sb_strlcpy(res->cname, tok[1], sizeof(res->cname));
		for (i = 2; i < nt; i++)
			res_alias(res, tok[i]);
		Close(fh);
		return 1;
	}
	Close(fh);
	return 0;
}

/* ------------------------------------------------------------------------
 * DNS client (server side)
 */

#define	T_A	1
#define	T_CNAME	5
#define	T_PTR	12
#define	T_AAAA	28
#define	C_IN	1

/* the wire form of a name (labels, final zero); -1 if it is not a name */
static int
dns_encode_name(UBYTE *p, int max, const char *name)
{
	int len = 0, i;

	while (*name) {
		for (i = 0; name[i] && name[i] != '.'; i++)
			;
		/* a name is at most 255 bytes on the wire (RFC 1035 3.1) */
		if (i == 0 || i > 63 || len + i + 2 > 255 || len + i + 2 > max)
			return -1;
		p[len++] = (UBYTE)i;
		while (i--)
			p[len++] = *name++;
		if (*name == '.')
			name++;
	}
	p[len++] = 0;
	return len;
}

/*
 * The name at pos in a message (labels and compression pointers, RFC
 * 1035 4.1.4) as text into out (NULL: just skip it); returns the position
 * after it in the message, or -1 if it is malformed.
 */
static int
dns_name(const UBYTE *msg, int msglen, int pos, char *out, int outlen)
{
	int end = -1, o = 0, hops = 0, l;

	for (;;) {
		if (pos < 0 || pos >= msglen)
			return -1;
		l = msg[pos];
		if ((l & 0xc0) == 0xc0) {
			if (pos + 1 >= msglen || ++hops > 16)
				return -1;
			if (end < 0)
				end = pos + 2;
			pos = ((l & 0x3f) << 8) | msg[pos + 1];
			continue;
		}
		if (l & 0xc0)
			return -1;		/* reserved label type */
		pos++;
		if (l == 0)
			break;
		if (pos + l > msglen)
			return -1;
		if (out) {
			if (o + l + 2 > outlen)
				return -1;
			if (o)
				out[o++] = '.';
			while (l--)
				out[o++] = msg[pos++];
		} else
			pos += l;
	}
	if (out)
		out[o] = '\0';
	return end < 0 ? pos : end;
}

/* is this a reply to our query? (RFC 5452 section 9.1: the reply must
   echo the query's ID and question) */
static int
dns_valid_reply(const UBYTE *r, int n, UWORD id, const UBYTE *q, int qlen)
{
	char a[256], b[256];
	int pa, pb, i;

	if (n < 12 || r[0] != (UBYTE)(id >> 8) || r[1] != (UBYTE)id)
		return 0;
	if (!(r[2] & 0x80) || ((r[2] >> 3) & 15) != 0)	/* QR, opcode 0 */
		return 0;
	if (((r[4] << 8) | r[5]) == 0)
		/* an error answer may leave the question out (FORMERR) */
		return (r[3] & 15) != 0;
	if (((r[4] << 8) | r[5]) != 1)
		return 0;
	pa = dns_name(r, n, 12, a, sizeof(a));
	pb = dns_name(q, qlen, 12, b, sizeof(b));
	if (pa < 0 || pb < 0 || pa + 4 > n || sb_strcasecmp(a, b) != 0)
		return 0;
	for (i = 0; i < 4; i++)			/* type and class */
		if (r[pa + i] != q[pb + i])
			return 0;
	return 1;
}

/* waiting for an answer: the time left of one attempt */
struct dns_wait {
	struct sbtime left;
	ULONG start;
};

static void
dns_wait_start(struct dns_wait *w)
{

	w->left.s = DNS_TIMEOUT_S;
	w->left.ms = 0;
	w->start = amiga_host_ms();
}

static LONG
dns_wait(struct SocketBase *sb, LONG fd, int what, struct dns_wait *w)
{
	LONG e = sb_wait_fd(sb, fd, what, &w->left);
	ULONG now = amiga_host_ms();

	sbtime_sub(&w->left, now - w->start);
	w->start = now;
	return e;
}

/*
 * Send the query to one server and wait for its answer: 0 with the
 * message in r (length *n); 1 if none arrives in time; -1 if the call
 * was interrupted.  Messages from any other address or port, and ones
 * that do not echo the query, are ignored without extending the wait.
 */
static int
dns_udp(struct SocketBase *sb, LONG s, ULONG server, const UBYTE *q, int qlen,
    UWORD id, UBYTE *r, int rsz, int *n)
{
	struct sockaddr_in sin, from;
	socklen_t fl;
	struct dns_wait w;
	LONG e, got;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = DNS_PORT;
	sin.sin_addr.s_addr = server;
	if (rump___sysimpl_sendto(s, q, qlen, 0, &sin, sizeof(sin)) < 0) {
		DNSDBG("dns: sendto %lx failed errno %ld\n", server,
		    (long)sb_rumperr());
		return 1;
	}
	dns_wait_start(&w);
	for (;;) {
		if ((e = dns_wait(sb, s, WAIT_READ, &w)) == EINTR)
			return -1;
		if (e)
			return 1;
		fl = sizeof(from);
		got = rump___sysimpl_recvfrom(s, r, rsz, 0, &from, &fl);
		if (got < 0) {
			if (sb_rumperr() == EWOULDBLOCK)
				continue;
			return 1;
		}
		if (from.sin_addr.s_addr != server || from.sin_port != DNS_PORT)
			continue;
		if (!dns_valid_reply(r, got, id, q, qlen))
			continue;
		*n = got;
		return 0;
	}
}

/* exactly len bytes from / to a connected stream socket within w's time:
   0, 1 (failed or timed out) or -1 (interrupted) */
static int
tcp_xfer(struct SocketBase *sb, LONG s, UBYTE *buf, int len, int send,
    struct dns_wait *w)
{
	int done = 0;
	LONG n, e;

	while (done < len) {
		if (send)
			n = rump___sysimpl_sendto(s, buf + done, len - done,
			    NB_MSG_NOSIGNAL, NULL, 0);
		else
			n = rump___sysimpl_recvfrom(s, buf + done, len - done, 0,
			    NULL, NULL);
		if (n > 0) {
			done += n;
			continue;
		}
		if (n == 0 || sb_rumperr() != EWOULDBLOCK)
			return 1;
		if ((e = dns_wait(sb, s, send ? WAIT_WRITE : WAIT_READ,
		    w)) == EINTR)
			return -1;
		if (e)
			return 1;
	}
	return 0;
}

/* the same over TCP (RFC 1035 4.2.2: a two byte length, then the
   message), for answers that did not fit a datagram */
static int
dns_tcp(struct SocketBase *sb, ULONG server, const UBYTE *q, int qlen,
    UWORD id, UBYTE *r, int rsz, int *n)
{
	struct sockaddr_in sin;
	struct dns_wait w;
	UBYTE lenbuf[2], *msg;
	LONG s, on = 1, err, e;
	socklen_t el = sizeof(err);
	int rc = 1, mlen;

	if ((s = rump___sysimpl_socket30(AF_INET, SOCK_STREAM, 0)) < 0)
		return 1;
	rump___sysimpl_ioctl(s, FIONBIO, &on);
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = DNS_PORT;
	sin.sin_addr.s_addr = server;
	dns_wait_start(&w);
	if (rump___sysimpl_connect(s, &sin, sizeof(sin)) < 0) {
		if (sb_rumperr() != EINPROGRESS)
			goto out;
		if ((e = dns_wait(sb, s, WAIT_WRITE, &w)) != 0) {
			if (e == EINTR)
				rc = -1;
			goto out;
		}
		if (rump___sysimpl_getsockopt(s, SOL_SOCKET, SO_ERROR, &err,
		    &el) < 0 || err)
			goto out;
	}
	if ((msg = AllocVec(qlen + 2, MEMF_PUBLIC)) == NULL)
		goto out;
	msg[0] = qlen >> 8;
	msg[1] = qlen;
	CopyMem((APTR)q, msg + 2, qlen);
	rc = tcp_xfer(sb, s, msg, qlen + 2, 1, &w);
	FreeVec(msg);
	if (rc != 0)
		goto out;
	if ((rc = tcp_xfer(sb, s, lenbuf, 2, 0, &w)) != 0)
		goto out;
	mlen = (lenbuf[0] << 8) | lenbuf[1];
	if (mlen > rsz || mlen < 12) {
		rc = 1;
		goto out;
	}
	if ((rc = tcp_xfer(sb, s, r, mlen, 0, &w)) != 0)
		goto out;
	if (!dns_valid_reply(r, mlen, id, q, qlen)) {
		rc = 1;
		goto out;
	}
	*n = mlen;
out:
	rump___sysimpl_close(s);
	return rc;
}

/* a record of the answer section */
struct dns_rr {
	int own;	/* position of the owner name */
	int type;
	int rdpos, rdlen;
};

/*
 * Pick the answer out of a validated reply (RFC 1035 4.1): follow the
 * CNAME chain from the queried name; only records owned by the name at
 * the end of the chain count.  Returns 0 or an h_errno value.
 */
static int
dns_parse(const UBYTE *r, int n, const char *qname, int qtype,
    struct dns_result *res)
{
	struct dns_rr rr[64];
	char cur[256], ob[256], nb[256];
	int rcode = r[3] & 15, qd = (r[4] << 8) | r[5];
	int an = (r[6] << 8) | r[7], pos = 12, nrr = 0, i, pass, changed;
	int found = 0;

	if (rcode == 3)
		return HOST_NOT_FOUND;		/* NXDOMAIN */
	if (rcode != 0)
		return NO_RECOVERY;		/* FORMERR, NOTIMP, REFUSED */
	while (qd-- > 0) {
		if ((pos = dns_name(r, n, pos, NULL, 0)) < 0 || pos + 4 > n)
			return NO_RECOVERY;
		pos += 4;
	}
	while (an-- > 0 && nrr < 64) {
		struct dns_rr *x = &rr[nrr];
		int np, cls;

		x->own = pos;
		if ((np = dns_name(r, n, pos, NULL, 0)) < 0 || np + 10 > n)
			break;
		x->type = (r[np] << 8) | r[np + 1];
		cls = (r[np + 2] << 8) | r[np + 3];
		x->rdlen = (r[np + 8] << 8) | r[np + 9];
		x->rdpos = np + 10;
		if (x->rdpos + x->rdlen > n)
			break;
		pos = x->rdpos + x->rdlen;
		if (cls == C_IN)
			nrr++;
	}
	sb_strlcpy(cur, qname, sizeof(cur));
	sb_strlcpy(res->cname, qname, sizeof(res->cname));
	for (pass = 0; pass < 16; pass++) {
		changed = 0;
		for (i = 0; i < nrr; i++) {
			if (rr[i].type != T_CNAME ||
			    dns_name(r, n, rr[i].own, ob, sizeof(ob)) < 0 ||
			    sb_strcasecmp(ob, cur) != 0 ||
			    dns_name(r, n, rr[i].rdpos, nb, sizeof(nb)) < 0)
				continue;
			/* the name asked for becomes an alias of the one
			   the chain ends in */
			sb_strlcpy(res->cname, nb, sizeof(res->cname));
			res_alias(res, cur);
			sb_strlcpy(cur, nb, sizeof(cur));
			changed = 1;
			break;
		}
		if (!changed)
			break;
	}
	for (i = 0; i < nrr; i++) {
		if (rr[i].type != qtype ||
		    dns_name(r, n, rr[i].own, ob, sizeof(ob)) < 0 ||
		    sb_strcasecmp(ob, cur) != 0)
			continue;
		if (qtype == T_A && rr[i].rdlen == 4 && res->naddr < 8) {
			CopyMem((APTR)(r + rr[i].rdpos), res->addr[res->naddr++], 4);
			found = 1;
		} else if (qtype == T_AAAA && rr[i].rdlen == 16 &&
		    res->naddr < 8) {
			CopyMem((APTR)(r + rr[i].rdpos), res->addr[res->naddr++],
			    16);
			found = 1;
		} else if (qtype == T_PTR && !found &&
		    dns_name(r, n, rr[i].rdpos, res->ptrname,
		    sizeof(res->ptrname)) >= 0)
			found = 1;
	}
	return found ? 0 : NO_DATA;
}

/* a random 16 bit query ID from the kernel's generator (kern.arnd, the
   NetBSD cprng; netbsd-src/sys/kern/subr_cprng.c) */
static int
dns_random_id(UWORD *id)
{
	LONG mib[2];
	ULONG v, len = sizeof(v);

	mib[0] = NB_CTL_KERN;
	mib[1] = NB_KERN_ARND;
	if (nb_sysctl(mib, 2, &v, &len, NULL, 0) != 0 || len != sizeof(v))
		return -1;
	*id = (UWORD)v;
	return 0;
}

/*
 * Ask the name servers; returns 0 with results, or an h_errno value
 * (NETDB_INTERNAL, with the call failed, if it was interrupted or the
 * system failed).
 */
static int
dns_query(struct SocketBase *sb, const char *name, int qtype,
    struct dns_result *res)
{
	UBYTE q[300];
	UBYTE *r;
	ULONG ns[MAXNS];
	int nns, qlen, i, try, rv = TRY_AGAIN, n = 0, rc;
	LONG s, on = 1;
	UWORD id;

	if ((nns = netdb_get_nameservers(ns, MAXNS)) == 0)
		return NO_RECOVERY;
	if (dns_random_id(&id) != 0) {
		sb_fail(sb, sb_rumperr());
		return NETDB_INTERNAL;
	}
	memset(q, 0, 12);
	q[0] = id >> 8; q[1] = id;
	q[2] = 0x01;			/* RD */
	q[5] = 1;			/* QDCOUNT */
	if ((qlen = dns_encode_name(q + 12, sizeof(q) - 16, name)) < 0)
		return NO_RECOVERY;
	qlen += 12;
	q[qlen++] = 0; q[qlen++] = qtype;
	q[qlen++] = 0; q[qlen++] = C_IN;

	if ((r = AllocVec(DNS_REPLY_MAX, MEMF_PUBLIC)) == NULL) {
		sb_fail(sb, ENOMEM);
		return NETDB_INTERNAL;
	}
	if ((s = rump___sysimpl_socket30(AF_INET, SOCK_DGRAM, 0)) < 0) {
		sb_fail(sb, sb_rumperr());
		FreeVec(r);
		return NETDB_INTERNAL;
	}
	rump___sysimpl_ioctl(s, FIONBIO, &on);
	for (try = 0; try < DNS_TRIES; try++) {
		for (i = 0; i < nns; i++) {
			DNSDBG("dns: query %s type %d to %lx\n", name, qtype,
			    ns[i]);
			rc = dns_udp(sb, s, ns[i], q, qlen, id, r,
			    DNS_REPLY_MAX, &n);
			if (rc == 0 && (r[2] & 0x02))	/* TC: truncated */
				rc = dns_tcp(sb, ns[i], q, qlen, id, r,
				    DNS_REPLY_MAX, &n);
			if (rc < 0) {
				rump___sysimpl_close(s);
				FreeVec(r);
				sb_fail(sb, EINTR);
				return NETDB_INTERNAL;
			}
			if (rc != 0)
				continue;	/* no answer: next server */
			if ((r[3] & 15) == 2) {	/* SERVFAIL: try another */
				rv = TRY_AGAIN;
				res->servfail = 1;
				continue;
			}
			DNSDBG("dns: answer len %ld rcode %d\n", (long)n,
			    r[3] & 15);
			rump___sysimpl_close(s);
			rv = dns_parse(r, n, name, qtype, res);
			FreeVec(r);
			return rv;
		}
	}
	rump___sysimpl_close(s);
	FreeVec(r);
	return rv;
}

/* name -> IPv4 addresses: numeric, localhost, hosts file, DNS */
static int
resolve4(struct SocketBase *sb, const char *name, struct dns_result *res)
{
	ULONG a;
	char bare[256], fq[256], dom[256];
	size_t l = sb_strlen(name);
	int rv, absolute;

	res_clear(res);
	if (parse_inet_aton(name, &a)) {
		sb_strlcpy(res->cname, name, sizeof(res->cname));
		res->naddr = 1;
		CopyMem(&a, res->addr[0], 4);
		return 0;
	}
	if (l == 0)
		return HOST_NOT_FOUND;
	if (l >= sizeof(bare))
		return NO_RECOVERY;
	/* "the name ends in a dot": fully qualified (the doc,
	   gethostbyname) */
	absolute = name[l - 1] == '.';
	sb_strlcpy(bare, name, sizeof(bare));
	if (absolute)
		bare[l - 1] = '\0';
	if (sb_strcasecmp(bare, "localhost") == 0) {
		a = INADDR_LOOPBACK;
		sb_strlcpy(res->cname, "localhost", sizeof(res->cname));
		res->naddr = 1;
		CopyMem(&a, res->addr[0], 4);
		return 0;
	}
	if (hosts_lookup_name(bare, res) > 0)
		return 0;
	/*
	 * "the current domain and its parents unless the name ends in a
	 * dot" (the doc, gethostbyname), in the order of NetBSD's resolver
	 * with its defaults (downloads/sources/netbsd-resolv: res_query.c
	 * res_nsearch(); ndots 1, RES_DEFNAMES and RES_DNSRCH, res_init.c
	 * 266 and resolv.h:259-260): a name with a dot is asked as it is
	 * first; then the name in the search list, which is the default
	 * domain and its parents that still have LOCALDOMAINPARTS (2)
	 * labels, at most MAXDFLSRCH (3) in all (res_init.c:535-552,
	 * resolv.h:135-137); a name without a dot as it is last.  The
	 * search goes on after "no such name", "no data" and a server
	 * failure (SERVFAIL, res_query.c:327-348), and stops at anything
	 * else, also TRY_AGAIN without a SERVFAIL answer (no answer at all,
	 * dns_query()).
	 */
	{
		const char *srch[3], *cp;
		int dots = 0, ddots = 0, n = 0, i, tried = 0, saved = -1;
		int nodata = 0, servfail = 0;

		for (cp = bare; *cp; cp++)
			dots += *cp == '.';
		if (dots >= 1 || absolute) {
			res_clear(res);
			rv = dns_query(sb, bare, T_A, res);
			if (rv == 0 || absolute || rv == NETDB_INTERNAL)
				return rv;
			saved = rv;
			tried = 1;
		}
		/* (a copy: DHCP may change it meanwhile) */
		ns_lock();
		sb_strlcpy(dom, domainname, sizeof(dom));
		ReleaseSemaphore(&nslock);
		if (dom[0]) {
			srch[n++] = dom;
			for (cp = dom; *cp; cp++)
				ddots += *cp == '.';
			for (cp = dom; n < 3 && ddots >= 2; ddots--) {
				while (*cp != '.')
					cp++;
				srch[n++] = ++cp;
			}
		}
		for (i = 0; i < n; i++) {
			if (sb_strlen(bare) + sb_strlen(srch[i]) + 2 >
			    sizeof(fq))
				continue;
			sb_strlcpy(fq, bare, sizeof(fq));
			fq[sb_strlen(bare)] = '.';
			sb_strlcpy(fq + sb_strlen(bare) + 1, srch[i],
			    sizeof(fq) - sb_strlen(bare) - 1);
			res_clear(res);
			rv = dns_query(sb, fq, T_A, res);
			if (rv == 0 || rv == NETDB_INTERNAL)
				return rv;
			if (rv == NO_DATA)
				nodata = 1;
			else if (rv == TRY_AGAIN && res->servfail)
				servfail = 1;
			else if (rv != HOST_NOT_FOUND)
				break;
		}
		/* (as it is if not yet tried, also after a search that
		   stopped: downloads/sources/netbsd-resolv/res_query.c:
		   361-366) */
		if (!tried) {
			res_clear(res);
			rv = dns_query(sb, bare, T_A, res);
			if (rv == 0 || rv == NETDB_INTERNAL)
				return rv;
		}
		/* (the error reported: res_query.c:376-381) */
		if (saved != -1)
			return saved;
		if (nodata)
			return NO_DATA;
		if (servfail)
			return TRY_AGAIN;
		return rv;
	}
}

/* for the library's own users (interface and route calls); server side;
   0 with the first address */
int
netdb_resolve_addr(struct SocketBase *sb, const char *name, ULONG *addr)
{
	struct dns_result *res;
	int rv;

	if ((res = AllocVec(sizeof(*res), MEMF_PUBLIC)) == NULL)
		return -1;
	rv = resolve4(sb, name, res);
	if (rv == 0)
		CopyMem(res->addr[0], addr, 4);
	FreeVec(res);
	return rv;
}

/* ------------------------------------------------------------------------
 * hostent
 */

struct arena {
	char *p;
	size_t left;
};

static void *
ar_alloc(struct arena *a, size_t n, size_t align)
{
	size_t pad = (align - ((ULONG)a->p & (align - 1))) & (align - 1);
	void *r;

	if (n + pad > a->left)
		return NULL;
	r = a->p + pad;
	a->p += n + pad;
	a->left -= n + pad;
	return r;
}

/* one hostent with the first naddr addresses of a result in an arena */
static int
try_hostent(struct hostent *h, struct arena ar, const struct dns_result *res,
    int naddr)
{
	char *name, **aliases, **addrs;
	UBYTE *abuf;
	const char *a = res->aliasbuf;
	size_t l;
	int i;

	l = sb_strlen(res->cname) + 1;
	if ((name = ar_alloc(&ar, l, 1)) == NULL ||
	    (aliases = ar_alloc(&ar, (res->nalias + 1) * sizeof(char *), 4)) ==
	    NULL ||
	    (addrs = ar_alloc(&ar, (naddr + 1) * sizeof(char *), 4)) == NULL ||
	    (abuf = ar_alloc(&ar, naddr * 4, 4)) == NULL)
		return 0;
	CopyMem((APTR)res->cname, name, l);
	for (i = 0; i < res->nalias; i++) {
		l = sb_strlen(a) + 1;
		if ((aliases[i] = ar_alloc(&ar, l, 1)) == NULL)
			return 0;
		CopyMem((APTR)a, aliases[i], l);
		a += l;
	}
	aliases[res->nalias] = NULL;
	for (i = 0; i < naddr; i++) {
		CopyMem((APTR)res->addr[i], abuf + 4 * i, 4);
		addrs[i] = (char *)abuf + 4 * i;
	}
	addrs[naddr] = NULL;
	h->h_name = name;
	h->h_aliases = aliases;
	h->h_addrtype = AF_INET;
	h->h_length = 4;
	h->h_addr_list = addrs;
	return 1;
}

/* fill a hostent from a result, as many addresses as fit; NULL if not
   even one does */
static struct hostent *
fill_hostent(struct hostent *h, void *buf, size_t buflen,
    const struct dns_result *res)
{
	struct arena ar;
	int n;

	ar.p = buf;
	ar.left = buflen;
	for (n = res->naddr < 8 ? res->naddr : 8; n >= 1; n--)
		if (try_hostent(h, ar, res, n))
			return h;
	return NULL;
}

struct ghbn_args { const char *name; struct dns_result *res; };

static LONG
srv_resolve4(struct SocketBase *sb, struct ghbn_args *a)
{

	return resolve4(sb, a->name, a->res);
}

struct hostent *
sb_gethostbyname(struct SocketBase *sb, STRPTR name)
{
	struct dns_result *res;
	struct ghbn_args a;
	struct hostent *h = NULL;
	LONG he;

	if (name == NULL || (res = AllocVec(sizeof(*res), MEMF_PUBLIC)) == NULL) {
		sb_set_herrno(sb, NO_RECOVERY);
		return NULL;
	}
	a.name = (const char *)name;
	a.res = res;
	he = sb_rpc(sb, (sbfn_t)srv_resolve4, &a, RPC_INTERRUPTIBLE, NULL);
	if (he == 0)
		h = fill_hostent(&sb->hent, sb->hentbuf, sizeof(sb->hentbuf),
		    res);
	sb_set_herrno(sb, he);
	FreeVec(res);
	return h;
}

/* gethostbyname_r() and gethostbyaddr_r() results: in the caller's buffer;
   BSD's ERANGE (and NETDB_INTERNAL, "see errno", netdb.h) if it is too
   small */
static struct hostent *
finish_r(struct SocketBase *sb, struct hostent *hp, APTR buf, ULONG buflen,
    LONG e, const struct dns_result *res, LONG *he)
{
	struct hostent *h = NULL;

	if (e == 0) {
		if ((h = fill_hostent(hp, buf, buflen, res)) == NULL) {
			sb_set_errno(sb, ERANGE);
			e = NETDB_INTERNAL;
		}
	}
	if (he)
		*he = e;
	return h;
}

struct hostent *
sb_gethostbyname_r(struct SocketBase *sb, STRPTR name, struct hostent *hp,
    APTR buf, ULONG buflen, LONG *he)
{
	struct dns_result *res;
	struct ghbn_args a;
	struct hostent *h;
	LONG e;

	if (name == NULL || buf == NULL || hp == NULL) {
		sb_set_errno(sb, EFAULT);
		if (he)
			*he = NETDB_INTERNAL;
		return NULL;
	}
	if ((res = AllocVec(sizeof(*res), MEMF_PUBLIC)) == NULL) {
		sb_set_errno(sb, ENOMEM);
		if (he)
			*he = NETDB_INTERNAL;
		return NULL;
	}
	a.name = (const char *)name;
	a.res = res;
	e = sb_rpc(sb, (sbfn_t)srv_resolve4, &a, RPC_INTERRUPTIBLE, NULL);
	h = finish_r(sb, hp, buf, buflen, e, res, he);
	FreeVec(res);
	return h;
}

struct ghba_args { ULONG addr; struct dns_result *res; };

/* address -> name: hosts file, then a PTR query (in-addr.arpa) */
static LONG
srv_reverse4(struct SocketBase *sb, struct ghba_args *a)
{
	char q[64], *p = q;
	ULONG x = a->addr;
	int rv;

	res_clear(a->res);
	if (hosts_lookup_addr(x, a->res)) {
		a->res->naddr = 1;
		CopyMem(&x, a->res->addr[0], 4);
		return 0;
	}
	p = sb_fmt_ulong(p, x & 0xff); *p++ = '.';
	p = sb_fmt_ulong(p, (x >> 8) & 0xff); *p++ = '.';
	p = sb_fmt_ulong(p, (x >> 16) & 0xff); *p++ = '.';
	p = sb_fmt_ulong(p, x >> 24);
	sb_strlcpy(p, ".in-addr.arpa", sizeof(q) - (p - q));
	if ((rv = dns_query(sb, q, T_PTR, a->res)) != 0)
		return rv;
	sb_strlcpy(a->res->cname, a->res->ptrname, sizeof(a->res->cname));
	a->res->nalias = 0;
	a->res->naddr = 1;
	CopyMem(&x, a->res->addr[0], 4);
	return 0;
}

struct hostent *
sb_gethostbyaddr(struct SocketBase *sb, STRPTR addr, LONG len, LONG type)
{
	struct dns_result *res;
	struct ghba_args a;
	struct hostent *h = NULL;
	LONG he;

	if (type != AF_INET || len != 4 || addr == NULL) {
		sb_set_herrno(sb, NO_RECOVERY);
		return NULL;
	}
	if ((res = AllocVec(sizeof(*res), MEMF_PUBLIC)) == NULL) {
		sb_set_herrno(sb, NO_RECOVERY);
		return NULL;
	}
	CopyMem(addr, &a.addr, 4);
	a.res = res;
	he = sb_rpc(sb, (sbfn_t)srv_reverse4, &a, RPC_INTERRUPTIBLE, NULL);
	if (he == 0)
		h = fill_hostent(&sb->hent, sb->hentbuf, sizeof(sb->hentbuf),
		    res);
	sb_set_herrno(sb, he);
	FreeVec(res);
	return h;
}

struct hostent *
sb_gethostbyaddr_r(struct SocketBase *sb, STRPTR addr, LONG len, LONG type,
    struct hostent *hp, APTR buf, ULONG buflen, LONG *he)
{
	struct dns_result *res;
	struct ghba_args a;
	struct hostent *h;
	LONG e;

	if (addr == NULL || hp == NULL || buf == NULL) {
		sb_set_errno(sb, EFAULT);
		if (he)
			*he = NETDB_INTERNAL;
		return NULL;
	}
	if (type != AF_INET || len != 4) {
		if (he)
			*he = NO_RECOVERY;
		return NULL;
	}
	if ((res = AllocVec(sizeof(*res), MEMF_PUBLIC)) == NULL) {
		sb_set_errno(sb, ENOMEM);
		if (he)
			*he = NETDB_INTERNAL;
		return NULL;
	}
	CopyMem(addr, &a.addr, 4);
	a.res = res;
	e = sb_rpc(sb, (sbfn_t)srv_reverse4, &a, RPC_INTERRUPTIBLE, NULL);
	h = finish_r(sb, hp, buf, buflen, e, res, he);
	FreeVec(res);
	return h;
}

/* ------------------------------------------------------------------------
 * services and protocols databases
 */

/* the services of the built-in table, used when DEVS:Internet/services
   cannot be opened */
static const struct {
	const char *name;
	UWORD port;
	UBYTE tcp, udp;
} services[] = {
	{ "echo", 7, 1, 1 }, { "discard", 9, 1, 1 }, { "daytime", 13, 1, 1 },
	{ "ftp-data", 20, 1, 0 }, { "ftp", 21, 1, 0 }, { "ssh", 22, 1, 0 },
	{ "telnet", 23, 1, 0 }, { "smtp", 25, 1, 0 }, { "time", 37, 1, 1 },
	{ "whois", 43, 1, 0 }, { "domain", 53, 1, 1 }, { "bootps", 67, 0, 1 },
	{ "bootpc", 68, 0, 1 }, { "tftp", 69, 0, 1 }, { "gopher", 70, 1, 0 },
	{ "finger", 79, 1, 0 }, { "http", 80, 1, 0 }, { "www", 80, 1, 0 },
	{ "pop3", 110, 1, 0 }, { "sunrpc", 111, 1, 1 }, { "auth", 113, 1, 0 },
	{ "nntp", 119, 1, 0 }, { "ntp", 123, 0, 1 }, { "imap", 143, 1, 0 },
	{ "snmp", 161, 0, 1 }, { "irc", 194, 1, 0 }, { "ldap", 389, 1, 0 },
	{ "https", 443, 1, 0 }, { "syslog", 514, 0, 1 },
	{ "submission", 587, 1, 0 }, { "ftps", 990, 1, 0 },
	{ "imaps", 993, 1, 0 }, { "pop3s", 995, 1, 0 }, { "socks", 1080, 1, 0 },
	{ "nfs", 2049, 1, 1 }, { "ircd", 6667, 1, 0 },
	{ "http-alt", 8080, 1, 0 },
};
#define	NSERVICES	(sizeof(services) / sizeof(services[0]))

/* the Internet protocols (the doc, setprotoent: "Only the Internet
   protocols are currently understood"); numbers as netbsd-src/sys/netinet/
   in.h */
static const struct {
	const char *name;
	UBYTE num;
} protocols[] = {
	{ "ip", 0 }, { "icmp", 1 }, { "igmp", 2 }, { "tcp", 6 }, { "udp", 17 },
	{ "ipv6", 41 }, { "ipv6-icmp", 58 },
};
#define	NPROTOCOLS	(sizeof(protocols) / sizeof(protocols[0]))

/* one entry of the services database; the strings are in the caller's
   line buffer or in the built-in table */
struct svc {
	const char *name;
	UWORD port;
	const char *proto;
	char **aliases;
};

/* tokens of a services line; the base's alias array has room for this
   many less two */
#define	SVC_MAXTOK	34

static void
db_close(struct sbdb *db)
{

	if (db->mode == SBDB_FILE)
		Close(db->fh);
	db->mode = SBDB_CLOSED;
	db->fh = 0;
	db->stay = 0;
	db->pos = 0;
}

/* open (when closed) and rewind */
static void
db_rewind(struct sbdb *db)
{

	if (db->mode == SBDB_CLOSED) {
		if ((db->fh = Open((CONST_STRPTR)SERVICES_FILE, MODE_OLDFILE)))
			db->mode = SBDB_FILE;
		else
			db->mode = SBDB_BUILTIN;
	} else if (db->mode == SBDB_FILE)
		Seek(db->fh, 0, OFFSET_BEGINNING);
	db->pos = 0;
}

/* the next entry, into 'line' (size) and 'aliases' (SVC_MAXTOK entries) */
static int
db_next(struct sbdb *db, char *line, int size, char **aliases, struct svc *s)
{
	char *tok[SVC_MAXTOK], *slash;
	int nt, i;
	ULONG port;

	if (db->mode == SBDB_FILE) {
		while ((nt = text_line(db->fh, line, size, tok, SVC_MAXTOK)) >=
		    0) {
			if (nt < 2)
				continue;
			for (slash = tok[1], port = 0; isdigit_(*slash); slash++)
				if ((port = port * 10 + (*slash - '0')) > 65535)
					break;
			if (slash == tok[1] || *slash != '/' || port > 65535 ||
			    slash[1] == '\0')
				continue;
			*slash++ = '\0';
			s->name = tok[0];
			s->port = (UWORD)port;
			s->proto = slash;
			for (i = 2; i < nt; i++)
				aliases[i - 2] = tok[i];
			aliases[nt > 2 ? nt - 2 : 0] = NULL;
			s->aliases = aliases;
			return 1;
		}
		return 0;
	}
	/* built-in: each service once per protocol, index 2 * entry + udp */
	while ((unsigned)db->pos < 2 * NSERVICES) {
		unsigned k = db->pos / 2;
		int udp = db->pos & 1;

		db->pos++;
		if (udp ? services[k].udp : services[k].tcp) {
			aliases[0] = NULL;
			s->name = services[k].name;
			s->port = services[k].port;
			s->proto = udp ? "udp" : "tcp";
			s->aliases = aliases;
			return 1;
		}
	}
	return 0;
}

/* a service by name, or by port when name is NULL; proto may be NULL for
   any; 1 if found */
static int
db_find(struct sbdb *db, char *line, int size, char **aliases,
    const char *name, UWORD port, const char *proto, struct svc *s)
{
	char **a;
	int found = 0;

	db_rewind(db);
	while (!found && db_next(db, line, size, aliases, s)) {
		if (proto != NULL && sb_strcmp(proto, s->proto) != 0)
			continue;
		if (name == NULL) {
			found = s->port == port;
			continue;
		}
		if (sb_strcmp(name, s->name) == 0)
			found = 1;
		for (a = s->aliases; !found && *a; a++)
			found = sb_strcmp(name, *a) == 0;
	}
	return found;
}

/* with its own open file, for the library's own lookups */
static int
svc_lookup(const char *name, UWORD port, const char *proto, struct svc *s,
    char *line, int size, char **aliases)
{
	struct sbdb db;
	int found;

	memset(&db, 0, sizeof(db));
	found = db_find(&db, line, size, aliases, name, port, proto, s);
	db_close(&db);
	return found;
}

struct serv_args {
	const char *name;
	LONG port;
	const char *proto;
	int op;
	LONG stay;
	struct servent *result;
};
#define	SVOP_BYNAME	0
#define	SVOP_BYPORT	1
#define	SVOP_NEXT	2
#define	SVOP_SET	3
#define	SVOP_END	4

static void
set_servent(struct SocketBase *sb, const struct svc *s)
{

	sb->sent.s_name = (char *)s->name;
	sb->sent.s_aliases = s->aliases;
	sb->sent.s_port = s->port;
	sb->sent.s_proto = (char *)s->proto;
}

static LONG
srv_serv(struct SocketBase *sb, struct serv_args *a)
{
	struct sbdb *db = &sb->db_serv;
	struct svc s;

	a->result = NULL;
	switch (a->op) {
	case SVOP_BYNAME:
	case SVOP_BYPORT:
		/* getservbyname() rewinds the database, and closes it again
		   unless setservent(stayopen) asked to keep it open (BSD) */
		if (db_find(db, sb->dbline, sizeof(sb->dbline), sb->dbaliases,
		    a->op == SVOP_BYNAME ? a->name : NULL, (UWORD)a->port,
		    a->proto, &s)) {
			set_servent(sb, &s);
			a->result = &sb->sent;
		}
		if (!db->stay)
			db_close(db);
		break;
	case SVOP_NEXT:
		if (db->mode == SBDB_CLOSED)
			db_rewind(db);
		if (db_next(db, sb->dbline, sizeof(sb->dbline), sb->dbaliases,
		    &s)) {
			set_servent(sb, &s);
			a->result = &sb->sent;
		}
		break;
	case SVOP_SET:
		db_rewind(db);
		db->stay = a->stay != 0;
		break;
	case SVOP_END:
		db_close(db);
		break;
	}
	return 0;
}

static struct servent *
serv_call(struct SocketBase *sb, int op, const char *name, LONG port,
    const char *proto, LONG stay)
{
	struct serv_args a;

	a.name = name;
	a.port = port;
	a.proto = proto;
	a.op = op;
	a.stay = stay;
	a.result = NULL;
	if (sb_rpc(sb, (sbfn_t)srv_serv, &a, 0, NULL) < 0)
		return NULL;
	return a.result;
}

struct servent *
sb_getservbyname(struct SocketBase *sb, STRPTR name, STRPTR proto)
{

	if (name == NULL)
		return NULL;
	return serv_call(sb, SVOP_BYNAME, (const char *)name, 0,
	    (const char *)proto, 0);
}

struct servent *
sb_getservbyport(struct SocketBase *sb, LONG port, STRPTR proto)
{

	return serv_call(sb, SVOP_BYPORT, NULL, port, (const char *)proto, 0);
}

void
sb_setservent(struct SocketBase *sb, LONG stayopen)
{

	serv_call(sb, SVOP_SET, NULL, 0, NULL, stayopen);
}

void
sb_endservent(struct SocketBase *sb)
{

	serv_call(sb, SVOP_END, NULL, 0, NULL, 0);
}

struct servent *
sb_getservent(struct SocketBase *sb)
{

	return serv_call(sb, SVOP_NEXT, NULL, 0, NULL, 0);
}

/* the base is closing: the file it still has open */
static LONG
srv_serv_end(struct SocketBase *sb, void *args)
{

	db_close(&sb->db_serv);
	return 0;
}

void
netdb_base_closed(struct SocketBase *sb)
{

	if (sb->db_serv.mode == SBDB_FILE)
		sb_rpc(sb, (sbfn_t)srv_serv_end, NULL, 0, NULL);
}

static struct protoent *
protoent_at(struct SocketBase *sb, unsigned i)
{

	sb->pentaliases[0] = NULL;
	sb->pent.p_name = (char *)protocols[i].name;
	sb->pent.p_aliases = sb->pentaliases;
	sb->pent.p_proto = protocols[i].num;
	return &sb->pent;
}

struct protoent *
sb_getprotobyname(struct SocketBase *sb, STRPTR name)
{
	unsigned i;

	if (name == NULL)
		return NULL;
	for (i = 0; i < NPROTOCOLS; i++)
		if (sb_strcasecmp(protocols[i].name, (const char *)name) == 0)
			return protoent_at(sb, i);
	return NULL;
}

struct protoent *
sb_getprotobynumber(struct SocketBase *sb, LONG proto)
{
	unsigned i;

	for (i = 0; i < NPROTOCOLS; i++)
		if (protocols[i].num == proto)
			return protoent_at(sb, i);
	return NULL;
}

void
sb_setprotoent(struct SocketBase *sb, LONG stayopen)
{

	sb->dbpos_proto = 0;
}

void
sb_endprotoent(struct SocketBase *sb)
{

	sb->dbpos_proto = 0;
}

struct protoent *
sb_getprotoent(struct SocketBase *sb)
{

	if ((unsigned)sb->dbpos_proto >= NPROTOCOLS)
		return NULL;
	return protoent_at(sb, sb->dbpos_proto++);
}

/*
 * The networks database: the doc describes the calls but names no file
 * for the data, so the database has no entries.
 */
struct netent *
sb_getnetbyname(struct SocketBase *sb, STRPTR name)
{

	return NULL;
}

struct netent *
sb_getnetbyaddr(struct SocketBase *sb, LONG net, LONG type)
{

	return NULL;
}

void
sb_setnetent(struct SocketBase *sb, LONG stayopen)
{
}

void
sb_endnetent(struct SocketBase *sb)
{
}

struct netent *
sb_getnetent(struct SocketBase *sb)
{

	return NULL;
}

int
netdb_lookup_net(struct SocketBase *sb, const char *name, ULONG *net)
{

	return 0;
}

/* ------------------------------------------------------------------------
 * getaddrinfo() family
 */

static struct addrinfo *
ai_new(int family, int socktype, int protocol, const UBYTE *addr, UWORD port,
    const char *canon)
{
	struct addrinfo *ai;
	size_t salen = family == AF_INET6 ? sizeof(struct sockaddr_in6) :
	    sizeof(struct sockaddr_in);
	size_t clen = canon ? sb_strlen(canon) + 1 : 0;

	ai = AllocVec(sizeof(*ai) + salen + clen, MEMF_PUBLIC | MEMF_CLEAR);
	if (ai == NULL)
		return NULL;
	ai->ai_family = family;
	ai->ai_socktype = socktype;
	ai->ai_protocol = protocol;
	ai->ai_addrlen = salen;
	ai->ai_addr = (struct sockaddr *)(ai + 1);
	if (family == AF_INET6) {
		struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ai->ai_addr;

		s6->sin6_len = salen;
		s6->sin6_family = AF_INET6;
		s6->sin6_port = port;
		CopyMem((APTR)addr, &s6->sin6_addr, 16);
	} else {
		struct sockaddr_in *s4 = (struct sockaddr_in *)ai->ai_addr;

		s4->sin_len = salen;
		s4->sin_family = AF_INET;
		s4->sin_port = port;
		CopyMem((APTR)addr, &s4->sin_addr, 4);
	}
	if (canon) {
		ai->ai_canonname = (char *)ai->ai_addr + salen;
		sb_strlcpy(ai->ai_canonname, canon, clen);
	}
	return ai;
}

void
sb_freeaddrinfo(struct SocketBase *sb, struct addrinfo *ai)
{
	struct addrinfo *next;

	for (; ai; ai = next) {
		next = ai->ai_next;
		FreeVec(ai);
	}
}

/* a decimal port number, all of the string */
static int
numeric_port(const char *s, UWORD *port)
{
	ULONG v = 0;

	if (!isdigit_(*s))
		return 0;
	for (; isdigit_(*s); s++)
		if ((v = v * 10 + (*s - '0')) > 65535)
			return 0;
	if (*s)
		return 0;
	*port = (UWORD)v;
	return 1;
}

struct gai_args {
	const char *host;	/* NULL: none */
	const char *serv;	/* NULL: none */
	int family, flags;
	int nst;		/* socket types asked for */
	int sts[2];
	int any_type;		/* the types were not given: any that has
				   the service */
	UWORD ports[2];
	struct dns_result *r4, *r6;
	int rv4, rv6;
	LONG eai;		/* a service error */
};

/* services: the port for each socket type, EAI_SERVICE if none */
static void
gai_services(struct gai_args *a)
{
	struct svc s;
	char line[512], *aliases[SVC_MAXTOK];
	UWORD port;
	int i, k = 0;

	if (a->serv == NULL) {
		for (i = 0; i < a->nst; i++)
			a->ports[i] = 0;
		return;
	}
	if (numeric_port(a->serv, &port)) {
		for (i = 0; i < a->nst; i++)
			a->ports[i] = port;
		return;
	}
	for (i = 0; i < a->nst; i++) {
		if (!svc_lookup(a->serv, 0, a->sts[i] == SOCK_DGRAM ? "udp" :
		    "tcp", &s, line, sizeof(line), aliases)) {
			if (a->any_type)
				continue;
			a->eai = EAI_SERVICE;
			return;
		}
		a->sts[k] = a->sts[i];
		a->ports[k++] = s.port;
	}
	if (k == 0)
		a->eai = EAI_SERVICE;
	a->nst = k;
}

static LONG
srv_gai(struct SocketBase *sb, struct gai_args *a)
{
	UBYTE t6[16];
	ULONG scratch;

	a->eai = 0;
	gai_services(a);
	if (a->eai)
		return 0;
	a->rv4 = a->rv6 = HOST_NOT_FOUND;
	if (a->host == NULL) {
		/* "if the nodename argument is a null pointer, then the IP
		   address portion of the socket address structure will be set
		   to INADDR_ANY" with AI_PASSIVE, "to the loopback address"
		   without (the doc, getaddrinfo) */
		if (a->family == AF_INET6) {
			a->r6->naddr = 1;
			if (!(a->flags & AI_PASSIVE))
				a->r6->addr[0][15] = 1;
			a->rv6 = 0;
		} else {
			ULONG any = (a->flags & AI_PASSIVE) ? INADDR_ANY :
			    INADDR_LOOPBACK;

			a->r4->naddr = 1;
			CopyMem(&any, a->r4->addr[0], 4);
			a->rv4 = 0;
		}
		return 0;
	}
	/* an IPv6 literal ("::1") is never looked up as an IPv4 name (that
	   would ask the name servers first, for nothing) */
	if ((a->family == AF_UNSPEC || a->family == AF_INET) &&
	    !pton6(a->host, t6)) {
		a->rv4 = resolve4(sb, a->host, a->r4);
		if (a->rv4 == NETDB_INTERNAL)
			return -1;
	}
	if (a->family == AF_UNSPEC || a->family == AF_INET6) {
		if (pton6(a->host, t6)) {
			res_clear(a->r6);
			sb_strlcpy(a->r6->cname, a->host, sizeof(a->r6->cname));
			a->r6->naddr = 1;
			CopyMem(t6, a->r6->addr[0], 16);
			a->rv6 = 0;
		} else if ((a->rv4 != 0 || a->family == AF_INET6) &&
		    !parse_inet_aton(a->host, &scratch)) {
			res_clear(a->r6);
			a->rv6 = dns_query(sb, a->host, T_AAAA, a->r6);
			if (a->rv6 == NETDB_INTERNAL)
				return -1;
		}
	}
	return 0;
}

/* the EAI_* code for a lookup that found nothing */
static LONG
eai_for(int rv4, int rv6)
{
	static const int pri[] = { TRY_AGAIN, NO_RECOVERY, NO_DATA };
	static const LONG eai[] = { EAI_AGAIN, EAI_FAIL, EAI_NODATA };
	unsigned i;

	for (i = 0; i < 3; i++)
		if (rv4 == pri[i] || rv6 == pri[i])
			return eai[i];
	return EAI_NONAME;
}

LONG
sb_getaddrinfo(struct SocketBase *sb, CONST_STRPTR hostname,
    CONST_STRPTR servname, const struct addrinfo *hints,
    struct addrinfo **res)
{
	int family = AF_UNSPEC, flags = 0, socktype = 0, protocol = 0;
	int i, j, k;
	UWORD port;
	struct addrinfo *head = NULL, **tail = &head, *ai;
	struct gai_args a;
	LONG rv = 0;
	ULONG scratch;
	UBYTE tmp[16];

	if (res == NULL) {
		sb_set_errno(sb, EFAULT);
		return EAI_SYSTEM;
	}
	*res = NULL;
	if (hints) {
		family = hints->ai_family;
		flags = hints->ai_flags;
		socktype = hints->ai_socktype;
		protocol = hints->ai_protocol;
		/* netdb.h: only the flags of AI_MASK are valid (there is no
		   AI_ADDRCONFIG in this API) */
		if (flags & ~AI_MASK)
			return EAI_BADFLAGS;
		/* "all members other than ai_flags, ai_family, ai_socktype,
		   and ai_protocol must be zero or a null pointer" (the doc,
		   getaddrinfo) */
		if (hints->ai_addrlen || hints->ai_addr ||
		    hints->ai_canonname || hints->ai_next)
			return EAI_BADHINTS;
	}
	if (hostname == NULL && servname == NULL)
		return EAI_NONAME;
	if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
		return EAI_FAMILY;
	if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM &&
	    socktype != SOCK_RAW)
		return EAI_SOCKTYPE;
	/* "getaddrinfo() will raise an error if members of the hints
	   structure are not consistent" (SOCK_STREAM with IPPROTO_UDP) */
	if ((socktype == SOCK_STREAM && protocol != 0 &&
	    protocol != IPPROTO_TCP) || (socktype == SOCK_DGRAM &&
	    protocol != 0 && protocol != IPPROTO_UDP))
		return EAI_BADHINTS;

	memset(&a, 0, sizeof(a));
	a.family = family;
	a.flags = flags;
	a.host = (const char *)hostname;
	a.serv = (const char *)servname;
	if (socktype)
		a.sts[a.nst++] = socktype;
	else if (protocol == IPPROTO_TCP)
		a.sts[a.nst++] = SOCK_STREAM;
	else if (protocol == IPPROTO_UDP)
		a.sts[a.nst++] = SOCK_DGRAM;
	else if (protocol != 0)
		a.sts[a.nst++] = SOCK_RAW;
	else {
		a.sts[a.nst++] = SOCK_STREAM;
		a.sts[a.nst++] = SOCK_DGRAM;
		a.any_type = 1;
	}
	if (servname != NULL) {
		/* "service names are not defined for the internet SOCK_RAW
		   space"; "a numeric servname ... does not identify any
		   socket type, and getaddrinfo() is not allowed to glob the
		   argument in such case" (the doc, getaddrinfo) */
		if (a.nst == 1 && a.sts[0] == SOCK_RAW)
			return EAI_SERVICE;
		if (numeric_port((const char *)servname, &port)) {
			if (a.any_type)
				return EAI_SERVICE;
		} else if (flags & AI_NUMERICSERV)
			return EAI_NONAME;
	}
	if (hostname != NULL && (flags & AI_NUMERICHOST) &&
	    !parse_inet_aton((const char *)hostname, &scratch) &&
	    !pton6((const char *)hostname, tmp))
		return EAI_NONAME;

	a.r4 = AllocVec(sizeof(struct dns_result), MEMF_PUBLIC | MEMF_CLEAR);
	a.r6 = AllocVec(sizeof(struct dns_result), MEMF_PUBLIC | MEMF_CLEAR);
	if (a.r4 == NULL || a.r6 == NULL) {
		rv = EAI_MEMORY;
		goto out;
	}
	if (sb_rpc(sb, (sbfn_t)srv_gai, &a, RPC_INTERRUPTIBLE, NULL) < 0) {
		/* interrupted (errno EINTR) or the system failed */
		rv = EAI_SYSTEM;
		goto out;
	}
	if (a.eai) {
		rv = a.eai;
		goto out;
	}
	for (k = 0; k < 2; k++) {
		struct dns_result *r = k ? a.r6 : a.r4;
		int fam = k ? AF_INET6 : AF_INET;

		if ((k ? a.rv6 : a.rv4) != 0)
			continue;
		for (i = 0; i < r->naddr; i++)
			for (j = 0; j < a.nst; j++) {
				int proto = protocol ? protocol :
				    a.sts[j] == SOCK_STREAM ? IPPROTO_TCP :
				    a.sts[j] == SOCK_DGRAM ? IPPROTO_UDP : 0;

				ai = ai_new(fam, a.sts[j], proto, r->addr[i],
				    a.ports[j],
				    (flags & AI_CANONNAME) && head == NULL &&
				    hostname != NULL ? r->cname : NULL);
				if (ai == NULL) {
					rv = EAI_MEMORY;
					goto out;
				}
				*tail = ai;
				tail = &ai->ai_next;
			}
	}
	if (head == NULL) {
		/* a literal of the other family than asked for */
		if (hostname != NULL && (family == AF_INET6 ?
		    parse_inet_aton((const char *)hostname, &scratch) :
		    family == AF_INET && pton6((const char *)hostname, tmp)))
			rv = EAI_ADDRFAMILY;
		else
			rv = eai_for(a.rv4, a.rv6);
	}
out:
	if (a.r4)
		FreeVec(a.r4);
	if (a.r6)
		FreeVec(a.r6);
	if (rv) {
		sb_freeaddrinfo(sb, head);
		return rv;
	}
	*res = head;
	return 0;
}

/* the texts are those of the catalog (MSG_BSDSOCKET_GAI_STRERROR_*) */
STRPTR
sb_gai_strerror(struct SocketBase *sb, LONG errnum)
{

	switch (errnum) {
	case 0: return (STRPTR)"no error";
	case EAI_BADFLAGS: return (STRPTR)"invalid value for ai_flags";
	case EAI_NONAME: return (STRPTR)"name or service is not known";
	case EAI_AGAIN: return (STRPTR)"temporary failure in name resolution";
	case EAI_FAIL: return (STRPTR)"non-recoverable failure in name resolution";
	case EAI_NODATA: return (STRPTR)"no address associated with name";
	case EAI_FAMILY: return (STRPTR)"ai_family not supported";
	case EAI_SOCKTYPE: return (STRPTR)"ai_socktype not supported";
	case EAI_SERVICE: return (STRPTR)"service not supported for ai_socktype";
	case EAI_ADDRFAMILY: return (STRPTR)"address family for name not supported";
	case EAI_MEMORY: return (STRPTR)"memory allocation failure";
	case EAI_SYSTEM: return (STRPTR)"system error";
	case EAI_BADHINTS: return (STRPTR)"invalid value for hints";
	case EAI_PROTOCOL: return (STRPTR)"resolved protocol is unknown";
	default: return (STRPTR)"unknown/invalid error";
	}
}

struct gni_args {
	const struct sockaddr *sa;
	ULONG flags;
	char *host;	/* NULL: not wanted */
	ULONG hostlen;
	char *serv;
	ULONG servlen;
	LONG eai;
};

/* the host part for an IPv4 address; 0, or -1 if interrupted */
static LONG
gni_host4(struct SocketBase *sb, struct gni_args *a, ULONG addr)
{
	struct dns_result *res = NULL;
	struct ghba_args g;
	char numeric[20], dom[256];
	const char *name = NULL;
	LONG he = HOST_NOT_FOUND;
	size_t keep, dl;

	if (!(a->flags & NI_NUMERICHOST)) {
		if ((res = AllocVec(sizeof(*res), MEMF_PUBLIC)) == NULL) {
			a->eai = EAI_MEMORY;
			return 0;
		}
		g.addr = addr;
		g.res = res;
		he = srv_reverse4(sb, &g);
		if (he == NETDB_INTERNAL) {
			FreeVec(res);
			return -1;
		}
		if (he == 0)
			name = res->cname;
	}
	if (name == NULL) {
		if (a->flags & NI_NAMEREQD) {
			a->eai = he == TRY_AGAIN ? EAI_AGAIN :
			    he == NO_RECOVERY ? EAI_FAIL : EAI_NONAME;
			goto out;
		}
		fmt_ip4(numeric, addr);
		name = numeric;
	}
	keep = sb_strlen(name);
	if (name != numeric && (a->flags & NI_NOFQDN)) {
		/* "only the nodename portion of the FQDN is returned for
		   local hosts": a name in the default domain loses it */
		ns_lock();
		sb_strlcpy(dom, domainname, sizeof(dom));
		ReleaseSemaphore(&nslock);
		dl = sb_strlen(dom);
		if (dl && keep > dl + 1 && name[keep - dl - 1] == '.' &&
		    sb_strcasecmp(name + keep - dl, dom) == 0)
			keep -= dl + 1;
	}
	if (keep + 1 > a->hostlen)
		a->eai = EAI_MEMORY;
	else {
		CopyMem((APTR)name, a->host, keep);
		a->host[keep] = '\0';
	}
out:
	if (res)
		FreeVec(res);
	return 0;
}

static LONG
srv_gni(struct SocketBase *sb, struct gni_args *a)
{
	UWORD port;

	a->eai = 0;
	if (a->sa->sa_family == AF_INET) {
		const struct sockaddr_in *s4 = (const struct sockaddr_in *)a->sa;

		port = s4->sin_port;
		if (a->host && gni_host4(sb, a, s4->sin_addr.s_addr) < 0)
			return -1;
	} else {
		const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)a->sa;

		port = s6->sin6_port;
		/* (there is no reverse lookup for IPv6: the numeric form) */
		if (a->host) {
			if (a->flags & NI_NAMEREQD)
				a->eai = EAI_NONAME;
			else if (ntop(AF_INET6, (const UBYTE *)&s6->sin6_addr,
			    (STRPTR)a->host, a->hostlen) != 0)
				a->eai = EAI_MEMORY;
		}
	}
	if (a->eai || a->serv == NULL)
		return 0;
	{
		struct svc s;
		char line[512], *aliases[SVC_MAXTOK], num[8];

		if (!(a->flags & NI_NUMERICSERV) &&
		    svc_lookup(NULL, port, (a->flags & NI_DGRAM) ? "udp" : "tcp",
		    &s, line, sizeof(line), aliases)) {
			if (sb_strlen(s.name) + 1 > a->servlen)
				a->eai = EAI_MEMORY;
			else
				sb_strlcpy(a->serv, s.name, a->servlen);
		} else {
			*sb_fmt_ulong(num, port) = '\0';
			if (sb_strlen(num) + 1 > a->servlen)
				a->eai = EAI_MEMORY;
			else
				sb_strlcpy(a->serv, num, a->servlen);
		}
	}
	return 0;
}

LONG
sb_getnameinfo(struct SocketBase *sb, const struct sockaddr *sa, ULONG salen,
    STRPTR host, ULONG hostlen, STRPTR serv, ULONG servlen, ULONG flags)
{
	struct gni_args a;

	if (sa == NULL) {
		sb_set_errno(sb, EFAULT);
		return EAI_SYSTEM;
	}
	if (flags & ~NI_ALL)
		return EAI_BADFLAGS;
	/* "The address family was not recognized or the address length was
	   invalid for the specified family" */
	if (sa->sa_family == AF_INET ? salen < sizeof(struct sockaddr_in) :
	    sa->sa_family == AF_INET6 ? salen < sizeof(struct sockaddr_in6) : 1)
		return EAI_FAMILY;
	a.sa = sa;
	a.flags = flags;
	a.host = host != NULL && hostlen != 0 ? (char *)host : NULL;
	a.hostlen = hostlen;
	a.serv = serv != NULL && servlen != 0 ? (char *)serv : NULL;
	a.servlen = servlen;
	/* "both nodename and servname were null" */
	if (a.host == NULL && a.serv == NULL)
		return EAI_NONAME;
	if (sb_rpc(sb, (sbfn_t)srv_gni, &a, RPC_INTERRUPTIBLE, NULL) < 0)
		return EAI_SYSTEM;
	return a.eai;
}

/* ------------------------------------------------------------------------
 * the domain name server API
 */

/* the doc, AddDomainNameServer: "the address must be given in
   dotted-decimal notation"; "adding the same address twice will require
   two calls RemoveDomainNameServer() to remove it again" */
LONG
sb_AddDomainNameServer(struct SocketBase *sb, STRPTR address)
{
	struct dnsadd *d, **dp;
	ULONG a;

	if (address == NULL) {
		sb_set_errno(sb, EFAULT);
		return -1;
	}
	if (!parse_dotted4((const char *)address, &a)) {
		sb_set_errno(sb, EINVAL);
		return -1;
	}
	ns_lock();
	for (dp = &dnsadded; (d = *dp) != NULL; dp = &d->next)
		if (d->addr == a) {
			d->count++;
			ReleaseSemaphore(&nslock);
			return 0;
		}
	if ((d = AllocVec(sizeof(*d), MEMF_PUBLIC)) == NULL) {
		ReleaseSemaphore(&nslock);
		sb_set_errno(sb, ENOBUFS);
		return -1;
	}
	d->next = NULL;
	d->addr = a;
	d->count = 1;
	*dp = d;
	ReleaseSemaphore(&nslock);
	return 0;
}

LONG
sb_RemoveDomainNameServer(struct SocketBase *sb, STRPTR address)
{
	struct dnsadd *d, **dp;
	ULONG a;

	if (address == NULL) {
		sb_set_errno(sb, EFAULT);
		return -1;
	}
	/* (an address that is no address is not in the database either) */
	if (parse_dotted4((const char *)address, &a)) {
		ns_lock();
		for (dp = &dnsadded; (d = *dp) != NULL; dp = &d->next)
			if (d->addr == a) {
				if (--d->count == 0) {
					*dp = d->next;
					FreeVec(d);
				}
				ReleaseSemaphore(&nslock);
				return 0;
			}
		ReleaseSemaphore(&nslock);
	}
	sb_set_errno(sb, ENOENT);
	return -1;
}

/* a node and the text of its address in one block */
struct dnsn_block {
	struct DomainNameServerNode node;
	char text[16];
};

void
sb_ReleaseDomainNameServerList(struct SocketBase *sb, struct List *list)
{
	struct Node *n;

	if (list == NULL)
		return;
	while ((n = RemHead(list)) != NULL)
		FreeVec(n);
	FreeVec(list);
}

static int
dnsn_add(struct List *list, ULONG addr, LONG count)
{
	struct dnsn_block *b;

	if ((b = AllocVec(sizeof(*b), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return 0;
	b->node.dnsn_Size = sizeof(b->node);
	fmt_ip4(b->text, addr);
	b->node.dnsn_Address = (STRPTR)b->text;
	b->node.dnsn_UseCount = count;
	AddTail(list, (struct Node *)&b->node.dnsn_MinNode);
	return 1;
}

/* the servers added with AddDomainNameServer() with their use counts,
   then the ones the stack configured, which have a negative use count
   (the SDK header, struct DomainNameServerNode) */
struct List *
sb_ObtainDomainNameServerList(struct SocketBase *sb)
{
	struct List *list;
	struct dnsadd *d;
	int i, ok = 1;

	if ((list = AllocVec(sizeof(*list), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return NULL;
	sb_newlist(list);
	ns_lock();
	for (d = dnsadded; d && ok; d = d->next)
		ok = dnsn_add(list, d->addr, d->count);
	for (i = 0; i < nstatic && ok; i++)
		ok = dnsn_add(list, static_ns[i], -1);
	ReleaseSemaphore(&nslock);
	if (!ok) {
		sb_ReleaseDomainNameServerList(sb, list);
		return NULL;
	}
	return list;
}

/* ------------------------------------------------------------------------
 * host identity
 */

LONG
sb_gethostname(struct SocketBase *sb, STRPTR name, LONG namelen)
{

	if (name == NULL || namelen <= 0) {
		sb_set_errno(sb, EFAULT);
		return -1;
	}
	sb_strlcpy((char *)name, hostname_cfg, namelen);
	return 0;
}

BOOL
sb_GetDefaultDomainName(struct SocketBase *sb, STRPTR buffer,
    LONG buffer_size)
{
	BOOL set;

	if (buffer == NULL || buffer_size <= 0)
		return FALSE;
	ns_lock();
	sb_strlcpy((char *)buffer, domainname, buffer_size);
	set = domainname[0] != '\0';
	ReleaseSemaphore(&nslock);
	return set;
}

void
sb_SetDefaultDomainName(struct SocketBase *sb, STRPTR buffer)
{

	netdb_set_domain((const char *)buffer);
}

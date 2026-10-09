/*
 * bsdsocket.library: address conversion, name resolution and the network
 * databases.
 *
 * Host names are resolved from the hosts file (DEVS:Internet/hosts, as
 * Roadshow uses), then by DNS over UDP to the configured name servers.
 * DNS queries run on the base's server thread (they need sockets and may
 * block), so they are interruptible with the break signal like any other
 * blocking call.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "sblib.h"

#define	HOSTS_FILE	"DEVS:Internet/hosts"
#define	MAXNS		4
#define	DNS_PORT	53
#define	DNS_TIMEOUT_MS	5000
#define	DNS_TRIES	3

/* resolver tracing in the stack log with DEBUG */
#define	DNSDBG(...)	do { if (amiga_rump_debug) amiga_rump_printf(__VA_ARGS__); } while (0)

static ULONG nameservers[MAXNS];
static int nnameservers;
static char domainname[128];
static char hostname_cfg[128] = "amiga";
static struct SignalSemaphore nslock;
static int nslock_init;

static void
ns_lock(void)
{

	Forbid();
	if (!nslock_init) {
		InitSemaphore(&nslock);
		nslock_init = 1;
	}
	Permit();
	ObtainSemaphore(&nslock);
}

void
netdb_set_nameservers(const ULONG *addrs, int n)
{
	int i;

	ns_lock();
	for (i = 0; i < n && i < MAXNS; i++)
		nameservers[i] = addrs[i];
	nnameservers = i;
	ReleaseSemaphore(&nslock);
}

int
netdb_get_nameservers(ULONG *addrs, int max)
{
	int i;

	ns_lock();
	for (i = 0; i < nnameservers && i < max; i++)
		addrs[i] = nameservers[i];
	ReleaseSemaphore(&nslock);
	return i;
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

/* BSD inet_aton(): a.b.c.d, a.b.c, a.b, a; decimal, octal or hex parts */
static int
parse_inet_aton(const char *cp, ULONG *res)
{
	ULONG parts[4], val;
	int n = 0, base, c;

	for (;;) {
		val = 0;
		base = 10;
		if (*cp == '0') {
			cp++;
			if (*cp == 'x' || *cp == 'X')
				base = 16, cp++;
			else
				base = 8;
		} else if (!isdigit_(*cp))
			return 0;
		while ((c = *cp) != 0) {
			if (isdigit_(c) && (base != 8 || c < '8'))
				val = val * base + (c - '0');
			else if (base == 16 && ((c >= 'a' && c <= 'f') ||
			    (c >= 'A' && c <= 'F')))
				val = val * 16 + ((c | 0x20) - 'a' + 10);
			else
				break;
			cp++;
		}
		if (*cp == '.') {
			if (n >= 3 || val > 0xff)
				return 0;
			parts[n++] = val;
			cp++;
			continue;
		}
		if (*cp && *cp != ' ' && *cp != '\t' && *cp != '\n')
			return 0;
		break;
	}
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

static char *
fmt_dec(char *p, ULONG v)
{
	char tmp[12];
	int i = 0;

	do {
		tmp[i++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (i)
		*p++ = tmp[--i];
	return p;
}

static void
fmt_ip4(char *buf, ULONG ip)
{
	char *p = buf;

	p = fmt_dec(p, ip >> 24); *p++ = '.';
	p = fmt_dec(p, (ip >> 16) & 0xff); *p++ = '.';
	p = fmt_dec(p, (ip >> 8) & 0xff); *p++ = '.';
	p = fmt_dec(p, ip & 0xff);
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

	return parse_inet_aton((const char *)cp, &v) ? v : INADDR_NONE;
}

LONG
sb_inet_aton(struct SocketBase *sb, STRPTR cp, struct in_addr *addr)
{
	ULONG v;

	if (!parse_inet_aton((const char *)cp, &v))
		return 0;
	if (addr)
		addr->s_addr = v;
	return 1;
}

/* classful helpers, kept for old software */
#define	IN_CLASSA(i)	(((ULONG)(i) & 0x80000000UL) == 0)
#define	IN_CLASSB(i)	(((ULONG)(i) & 0xc0000000UL) == 0x80000000UL)
#define	IN_CLASSC(i)	(((ULONG)(i) & 0xe0000000UL) == 0xc0000000UL)

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
	const char *p = (const char *)cp;
	ULONG v, net = 0;
	int n = 0;

	/* "a[.b[.c[.d]]]": each part one byte, most significant first */
	for (;;) {
		if (!isdigit_(*p) || n == 4)
			return INADDR_NONE;
		for (v = 0; isdigit_(*p); p++)
			v = v * 10 + (*p - '0');
		if (v > 255)
			return INADDR_NONE;
		net = (net << 8) | v;
		n++;
		if (*p != '.')
			break;
		p++;
	}
	return *p ? INADDR_NONE : net;
}

/* inet_ntop / inet_pton for AF_INET and AF_INET6 */
static const char hexd[] = "0123456789abcdef";

STRPTR
sb_inet_ntop(struct SocketBase *sb, LONG af, APTR src, STRPTR dst, LONG size)
{
	char buf[48], *p = buf;
	const UBYTE *a = src;
	int i, best = -1, bestlen = 0, cur = -1, curlen = 0;
	UWORD w[8];

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
				*p++ = ':';
				if (i == 0)
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
		if (best >= 0 && best + bestlen == 8 && p[-1] != ':')
			*p++ = ':';
		*p = '\0';
	} else {
		sb_set_errno(sb, EAFNOSUPPORT);
		return NULL;
	}
	if ((LONG)sb_strlen(buf) + 1 > size) {
		sb_set_errno(sb, 28 /* ENOSPC */);
		return NULL;
	}
	sb_strlcpy((char *)dst, buf, size);
	return dst;
}

static int
hexval(int c)
{

	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
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
			if (*q == '.' && n <= 6 && parse_inet_aton(s, &v4)) {
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
	const char *s = (const char *)src;
	UBYTE *d = dst;
	ULONG v = 0, part;
	int n;

	if (af == AF_INET) {
		/* strict dotted quad */
		for (n = 0; n < 4; n++) {
			if (!isdigit_(*s))
				return 0;
			part = 0;
			while (isdigit_(*s))
				part = part * 10 + (*s++ - '0');
			if (part > 255)
				return 0;
			v = (v << 8) | part;
			if (n < 3 && *s++ != '.')
				return 0;
		}
		if (*s)
			return 0;
		d[0] = v >> 24; d[1] = v >> 16; d[2] = v >> 8; d[3] = v;
		return 1;
	}
	if (af == AF_INET6)
		return pton6(s, d);
	sb_set_errno(sb, EAFNOSUPPORT);
	return -1;
}

/* ------------------------------------------------------------------------
 * hosts file
 */

/* look name up (or, with name NULL, the address addr) in the hosts file;
   fills canonical name and first address; returns 1 if found */
static int
hosts_lookup(const char *name, ULONG addr, char *cname, size_t cnlen,
    ULONG *addrp)
{
	char line[256];
	BPTR fh;
	int found = 0;

	if ((fh = Open((CONST_STRPTR)HOSTS_FILE, MODE_OLDFILE)) == 0)
		return 0;
	while (!found && FGets(fh, (STRPTR)line, sizeof(line))) {
		char *p = line, *tok[8];
		int nt = 0, i;
		ULONG a;

		while (nt < 8) {
			while (*p == ' ' || *p == '\t')
				p++;
			if (!*p || *p == '#' || *p == '\n' || *p == '\r')
				break;
			tok[nt++] = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
			    *p != '\r' && *p != '#')
				p++;
			if (*p == '#' || !*p) {
				*p = '\0';
				break;
			}
			*p++ = '\0';
		}
		if (nt < 2 || !parse_inet_aton(tok[0], &a))
			continue;
		if (name == NULL) {
			if (a == addr) {
				sb_strlcpy(cname, tok[1], cnlen);
				*addrp = a;
				found = 1;
			}
			continue;
		}
		for (i = 1; i < nt; i++) {
			if (sb_strcasecmp(tok[i], name) == 0) {
				sb_strlcpy(cname, tok[1], cnlen);
				*addrp = a;
				found = 1;
				break;
			}
		}
	}
	Close(fh);
	return found;
}

/* ------------------------------------------------------------------------
 * DNS client (server side)
 */

#define	T_A	1
#define	T_CNAME	5
#define	T_PTR	12
#define	T_AAAA	28

static int
dns_encode_name(UBYTE *p, int max, const char *name)
{
	int len = 0, i;

	while (*name) {
		for (i = 0; name[i] && name[i] != '.'; i++)
			;
		if (i == 0 || i > 63 || len + i + 2 > max)
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

/* expand a (possibly compressed) name at pos; returns bytes consumed */
static int
dns_expand(const UBYTE *msg, int msglen, int pos, char *out, int outlen)
{
	int consumed = -1, o = 0, hops = 0;

	while (pos < msglen) {
		int l = msg[pos];

		if ((l & 0xc0) == 0xc0) {
			if (pos + 1 >= msglen || ++hops > 16)
				return -1;
			if (consumed < 0)
				consumed = pos + 2;
			pos = ((l & 0x3f) << 8) | msg[pos + 1];
			continue;
		}
		pos++;
		if (l == 0)
			break;
		if (pos + l > msglen || o + l + 2 > outlen)
			return -1;
		if (o)
			out[o++] = '.';
		while (l--)
			out[o++] = msg[pos++];
	}
	out[o] = '\0';
	return consumed;	/* caller recomputes for the uncompressed case */
}

static int
dns_skip_name(const UBYTE *msg, int msglen, int pos)
{

	while (pos < msglen) {
		int l = msg[pos];

		if ((l & 0xc0) == 0xc0)
			return pos + 2;
		pos += l + 1;
		if (l == 0)
			return pos;
	}
	return -1;
}

struct dns_result {
	char cname[256];
	int naddr;
	UBYTE addr[8][16];
	char ptrname[256];
};

/*
 * Ask the name servers; returns 0 with results, or an h_errno value.
 * Runs on the server thread: may return EINTR-style abort as TRY_AGAIN.
 */
static int
dns_query(struct SocketBase *sb, const char *name, int qtype,
    struct dns_result *res)
{
	UBYTE q[300], r[1500];
	ULONG ns[MAXNS];
	int nns, qlen, i, try, rv = TRY_AGAIN;
	LONG s, n;
	UWORD id;
	struct sockaddr_in sin;
	socklen_t slen;
	struct EClockVal ev;

	if ((nns = netdb_get_nameservers(ns, MAXNS)) == 0)
		return NO_RECOVERY;
	ReadEClock(&ev);
	id = (UWORD)(ev.ev_lo ^ (ULONG)sb);

	memset(q, 0, 12);
	q[0] = id >> 8; q[1] = id;
	q[2] = 0x01;			/* RD */
	q[5] = 1;			/* QDCOUNT */
	if ((qlen = dns_encode_name(q + 12, sizeof(q) - 16, name)) < 0)
		return NO_RECOVERY;
	qlen += 12;
	q[qlen++] = 0; q[qlen++] = qtype;
	q[qlen++] = 0; q[qlen++] = 1;	/* IN */

	if ((s = rump___sysimpl_socket30(AF_INET, SOCK_DGRAM, 0)) < 0)
		return TRY_AGAIN;
	{
		LONG on = 1;

		rump___sysimpl_ioctl(s, FIONBIO, &on);
	}
	for (try = 0; try < DNS_TRIES; try++) {
		for (i = 0; i < nns; i++) {
			memset(&sin, 0, sizeof(sin));
			sin.sin_len = sizeof(sin);
			sin.sin_family = AF_INET;
			sin.sin_port = DNS_PORT;
			sin.sin_addr.s_addr = ns[i];
			if (rump___sysimpl_sendto(s, q, qlen, 0, &sin,
			    sizeof(sin)) < 0) {
				DNSDBG("dns: sendto %lx failed errno %ld\n", ns[i],
				    (long)sb_rumperr());
				continue;
			}
			DNSDBG("dns: query %s type %d to %lx\n", name, qtype,
			    ns[i]);
			for (;;) {
				LONG e = sb_wait_fd(sb, s, WAIT_READ,
				    DNS_TIMEOUT_MS);

				if (e == EINTR) {
					rump___sysimpl_close(s);
					sb_fail(sb, EINTR);
					return TRY_AGAIN;
				}
				if (e) {
					DNSDBG("dns: wait -> %ld\n", (long)e);
					break;	/* timeout: next server */
				}
				slen = sizeof(sin);
				n = rump___sysimpl_recvfrom(s, r, sizeof(r), 0,
				    &sin, &slen);
				if (n < 12 || r[0] != (id >> 8) ||
				    r[1] != (UBYTE)id || !(r[2] & 0x80))
					continue;	/* not our answer */
				goto answer;
			}
		}
	}
	rump___sysimpl_close(s);
	return TRY_AGAIN;

answer:
	rump___sysimpl_close(s);
	{
		int rcode = r[3] & 15, qd = (r[4] << 8) | r[5];
		int an = (r[6] << 8) | r[7], pos = 12;

		DNSDBG("dns: answer len %ld rcode %d qd %d an %d\n", (long)n,
		    rcode, qd, an);
		if (rcode == 3)
			return HOST_NOT_FOUND;
		if (rcode != 0)
			return rcode == 2 ? TRY_AGAIN : NO_RECOVERY;
		while (qd-- > 0 && pos >= 0)
			pos = dns_skip_name(r, n, pos) + 4;
		sb_strlcpy(res->cname, name, sizeof(res->cname));
		res->naddr = 0;
		res->ptrname[0] = '\0';
		rv = NO_DATA;
		while (an-- > 0 && pos > 0 && pos + 10 <= n) {
			int type, rdlen, np;

			np = dns_skip_name(r, n, pos);
			if (np < 0 || np + 10 > n)
				break;
			type = (r[np] << 8) | r[np + 1];
			rdlen = (r[np + 8] << 8) | r[np + 9];
			np += 10;
			if (np + rdlen > n)
				break;
			if (type == T_CNAME)
				dns_expand(r, n, np, res->cname,
				    sizeof(res->cname));
			else if (type == qtype && type == T_A && rdlen == 4 &&
			    res->naddr < 8) {
				CopyMem(r + np, res->addr[res->naddr++], 4);
				rv = 0;
			} else if (type == qtype && type == T_AAAA &&
			    rdlen == 16 && res->naddr < 8) {
				CopyMem(r + np, res->addr[res->naddr++], 16);
				rv = 0;
			} else if (type == T_PTR && qtype == T_PTR) {
				dns_expand(r, n, np, res->ptrname,
				    sizeof(res->ptrname));
				rv = 0;
			}
			pos = np + rdlen;
		}
	}
	return rv;
}

/* name -> IPv4 addresses: numeric, localhost, hosts file, DNS */
static int
resolve4(struct SocketBase *sb, const char *name, struct dns_result *res)
{
	ULONG a;
	char fq[256];
	int rv;

	if (parse_inet_aton(name, &a)) {
		sb_strlcpy(res->cname, name, sizeof(res->cname));
		res->naddr = 1;
		CopyMem(&a, res->addr[0], 4);
		return 0;
	}
	if (sb_strcasecmp(name, "localhost") == 0) {
		a = INADDR_LOOPBACK;
		sb_strlcpy(res->cname, "localhost", sizeof(res->cname));
		res->naddr = 1;
		CopyMem(&a, res->addr[0], 4);
		return 0;
	}
	if (hosts_lookup(name, 0, res->cname, sizeof(res->cname), &a)) {
		res->naddr = 1;
		CopyMem(&a, res->addr[0], 4);
		return 0;
	}
	rv = dns_query(sb, name, T_A, res);
	/* unqualified name: try the default domain as well */
	if (rv == HOST_NOT_FOUND && domainname[0]) {
		const char *p;

		for (p = name; *p && *p != '.'; p++)
			;
		if (!*p && sb_strlen(name) + sb_strlen(domainname) + 2 <
		    sizeof(fq)) {
			sb_strlcpy(fq, name, sizeof(fq));
			fq[sb_strlen(name)] = '.';
			sb_strlcpy(fq + sb_strlen(name) + 1, domainname,
			    sizeof(fq) - sb_strlen(name) - 1);
			rv = dns_query(sb, fq, T_A, res);
		}
	}
	return rv;
}

/* fill a hostent (AF_INET) from a result, using caller-supplied storage */
static struct hostent *
fill_hostent(struct hostent *h, char *namebuf, size_t namelen,
    char **aliases, char **addrs, UBYTE *addrbuf, int maxaddr,
    const struct dns_result *res)
{
	int i, n = res->naddr < maxaddr ? res->naddr : maxaddr;

	sb_strlcpy(namebuf, res->cname, namelen);
	aliases[0] = NULL;
	for (i = 0; i < n; i++) {
		CopyMem((APTR)res->addr[i], addrbuf + 4 * i, 4);
		addrs[i] = (char *)addrbuf + 4 * i;
	}
	addrs[n] = NULL;
	h->h_name = namebuf;
	h->h_aliases = aliases;
	h->h_addrtype = AF_INET;
	h->h_length = 4;
	h->h_addr_list = addrs;
	return h;
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

	if (name == NULL || (res = AllocVec(sizeof(*res), MEMF_ANY)) == NULL) {
		sb_set_herrno(sb, NO_RECOVERY);
		return NULL;
	}
	a.name = (const char *)name;
	a.res = res;
	he = sb_rpc(sb, (sbfn_t)srv_resolve4, &a, RPC_INTERRUPTIBLE, NULL);
	if (he == 0)
		h = fill_hostent(&sb->hent, sb->hent_name,
		    sizeof(sb->hent_name), sb->hent_aliases, sb->hent_addrs,
		    sb->hent_addrbuf, 8, res);
	sb_set_herrno(sb, he);
	FreeVec(res);
	return h;
}

struct hostent *
sb_gethostbyname_r(struct SocketBase *sb, STRPTR name, struct hostent *hp,
    APTR buf, ULONG buflen, LONG *he)
{
	struct dns_result *res;
	struct ghbn_args a;
	struct hostent *h = NULL;
	char *b = buf, **aliases, **addrs;
	LONG e;

	/* layout in buf: aliases[1], addrs[n+1], address bytes, name */
	if (buflen < 64 || (res = AllocVec(sizeof(*res), MEMF_ANY)) == NULL) {
		if (he)
			*he = NO_RECOVERY;
		return NULL;
	}
	a.name = (const char *)name;
	a.res = res;
	e = sb_rpc(sb, (sbfn_t)srv_resolve4, &a, RPC_INTERRUPTIBLE, NULL);
	if (e == 0) {
		int maxaddr = 4;

		while (maxaddr > 1 && (ULONG)(4 + 4 * (maxaddr + 1) +
		    4 * maxaddr + 32) > buflen)
			maxaddr--;
		aliases = (char **)b;
		addrs = aliases + 1;
		h = fill_hostent(hp, b + 4 + 4 * (maxaddr + 1) + 4 * maxaddr,
		    buflen - (4 + 4 * (maxaddr + 1) + 4 * maxaddr), aliases,
		    addrs, (UBYTE *)(addrs + maxaddr + 1), maxaddr, res);
	}
	if (he)
		*he = e;
	FreeVec(res);
	return h;
}

struct ghba_args { ULONG addr; struct dns_result *res; };

static LONG
srv_reverse4(struct SocketBase *sb, struct ghba_args *a)
{
	char q[64], *p = q;
	ULONG x = a->addr, dummy;

	if (hosts_lookup(NULL, x, a->res->cname, sizeof(a->res->cname),
	    &dummy)) {
		a->res->naddr = 1;
		CopyMem(&x, a->res->addr[0], 4);
		return 0;
	}
	p = fmt_dec(p, x & 0xff); *p++ = '.';
	p = fmt_dec(p, (x >> 8) & 0xff); *p++ = '.';
	p = fmt_dec(p, (x >> 16) & 0xff); *p++ = '.';
	p = fmt_dec(p, x >> 24);
	sb_strlcpy(p, ".in-addr.arpa", sizeof(q) - (p - q));
	if (dns_query(sb, q, T_PTR, a->res) != 0 || !a->res->ptrname[0])
		return HOST_NOT_FOUND;
	sb_strlcpy(a->res->cname, a->res->ptrname, sizeof(a->res->cname));
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
	if ((res = AllocVec(sizeof(*res), MEMF_ANY)) == NULL) {
		sb_set_herrno(sb, NO_RECOVERY);
		return NULL;
	}
	CopyMem(addr, &a.addr, 4);
	a.res = res;
	he = sb_rpc(sb, (sbfn_t)srv_reverse4, &a, RPC_INTERRUPTIBLE, NULL);
	if (he == 0)
		h = fill_hostent(&sb->hent, sb->hent_name,
		    sizeof(sb->hent_name), sb->hent_aliases, sb->hent_addrs,
		    sb->hent_addrbuf, 8, res);
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
	struct hostent *h = NULL;
	char **aliases = buf, **addrs;
	LONG e;

	if (type != AF_INET || len != 4 || buflen < 64 ||
	    (res = AllocVec(sizeof(*res), MEMF_ANY)) == NULL) {
		if (he)
			*he = NO_RECOVERY;
		return NULL;
	}
	CopyMem(addr, &a.addr, 4);
	a.res = res;
	e = sb_rpc(sb, (sbfn_t)srv_reverse4, &a, RPC_INTERRUPTIBLE, NULL);
	if (e == 0) {
		addrs = aliases + 1;
		h = fill_hostent(hp, (char *)buf + 20, buflen - 20, aliases,
		    addrs, (UBYTE *)buf + 12, 1, res);
	}
	if (he)
		*he = e;
	FreeVec(res);
	return h;
}

/* ------------------------------------------------------------------------
 * getaddrinfo() family
 */

#define	AI_PASSIVE	0x0001
#define	AI_CANONNAME	0x0002
#define	AI_NUMERICHOST	0x0004
#define	AI_NUMERICSERV	0x0008
#define	EAI_AGAIN	2
#define	EAI_BADFLAGS	3
#define	EAI_FAIL	4
#define	EAI_FAMILY	5
#define	EAI_MEMORY	6
#define	EAI_NONAME	8
#define	EAI_SERVICE	9
#define	EAI_SOCKTYPE	10
#define	EAI_SYSTEM	11
#define	NI_NUMERICHOST	0x0002
#define	NI_NAMEREQD	0x0004
#define	NI_NUMERICSERV	0x0008
#define	NI_DGRAM	0x0010

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

static const struct {
	const char *name;
	UBYTE num;
} protocols[] = {
	{ "ip", 0 }, { "icmp", 1 }, { "igmp", 2 }, { "tcp", 6 }, { "udp", 17 },
	{ "ipv6", 41 }, { "ipv6-icmp", 58 },
};
#define	NPROTOCOLS	(sizeof(protocols) / sizeof(protocols[0]))

static int
service_port(const char *serv, int socktype, UWORD *port)
{
	ULONG v = 0;
	const char *p;
	unsigned i;

	for (p = serv; isdigit_(*p); p++)
		v = v * 10 + (*p - '0');
	if (!*p && p != serv) {
		if (v > 65535)
			return 0;
		*port = (UWORD)v;
		return 1;
	}
	for (i = 0; i < NSERVICES; i++)
		if (sb_strcmp(services[i].name, serv) == 0 &&
		    (socktype == SOCK_DGRAM ? services[i].udp :
		    socktype == SOCK_STREAM ? services[i].tcp : 1)) {
			*port = services[i].port;
			return 1;
		}
	return 0;
}

static struct addrinfo *
ai_new(int family, int socktype, int protocol, const UBYTE *addr, UWORD port,
    const char *canon)
{
	struct addrinfo *ai;
	size_t salen = family == AF_INET6 ? sizeof(struct sockaddr_in6) :
	    sizeof(struct sockaddr_in);
	size_t clen = canon ? sb_strlen(canon) + 1 : 0;

	ai = AllocVec(sizeof(*ai) + salen + clen, MEMF_ANY | MEMF_CLEAR);
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

struct gai_args {
	const char *host;
	int family;
	struct dns_result *r4, *r6;
	int rv4, rv6;
};

static LONG
srv_gai(struct SocketBase *sb, struct gai_args *a)
{

	a->rv4 = a->rv6 = HOST_NOT_FOUND;
	if (a->family == AF_UNSPEC || a->family == AF_INET)
		a->rv4 = resolve4(sb, a->host, a->r4);
	if (a->family == AF_UNSPEC || a->family == AF_INET6) {
		UBYTE tmp[16];

		if (pton6(a->host, tmp)) {
			sb_strlcpy(a->r6->cname, a->host, sizeof(a->r6->cname));
			a->r6->naddr = 1;
			CopyMem(tmp, a->r6->addr[0], 16);
			a->rv6 = 0;
		} else if (a->rv4 != 0 || a->family == AF_INET6) {
			ULONG dummy;

			if (!parse_inet_aton(a->host, &dummy))
				a->rv6 = dns_query(sb, a->host, T_AAAA, a->r6);
		}
	}
	return 0;
}

LONG
sb_getaddrinfo(struct SocketBase *sb, CONST_STRPTR hostname,
    CONST_STRPTR servname, const struct addrinfo *hints,
    struct addrinfo **res)
{
	int family = hints ? hints->ai_family : AF_UNSPEC;
	int flags = hints ? hints->ai_flags : 0;
	int socktypes[2], nst = 0, i, j, k;
	int protocol = hints ? hints->ai_protocol : 0;
	UWORD port = 0;
	struct addrinfo *head = NULL, **tail = &head, *ai;
	struct gai_args a;
	LONG rv = 0;

	*res = NULL;
	if (hostname == NULL && servname == NULL)
		return EAI_NONAME;
	if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
		return EAI_FAMILY;
	if (hints && hints->ai_socktype)
		socktypes[nst++] = hints->ai_socktype;
	else {
		socktypes[nst++] = SOCK_STREAM;
		socktypes[nst++] = SOCK_DGRAM;
	}
	if (servname && !service_port((const char *)servname,
	    nst == 1 ? socktypes[0] : 0, &port))
		return EAI_SERVICE;

	memset(&a, 0, sizeof(a));
	a.family = family;
	a.r4 = AllocVec(sizeof(struct dns_result), MEMF_ANY | MEMF_CLEAR);
	a.r6 = AllocVec(sizeof(struct dns_result), MEMF_ANY | MEMF_CLEAR);
	if (a.r4 == NULL || a.r6 == NULL) {
		rv = EAI_MEMORY;
		goto out;
	}
	if (hostname == NULL) {
		ULONG any = (flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK;

		a.r4->naddr = 1;
		CopyMem(&any, a.r4->addr[0], 4);
		a.rv6 = HOST_NOT_FOUND;
		if (family == AF_INET6) {
			a.r6->naddr = 1;
			memset(a.r6->addr[0], 0, 16);
			if (!(flags & AI_PASSIVE))
				a.r6->addr[0][15] = 1;
			a.rv6 = 0;
			a.rv4 = HOST_NOT_FOUND;
		}
	} else {
		ULONG dummy;
		UBYTE tmp[16];

		if ((flags & AI_NUMERICHOST) &&
		    !parse_inet_aton((const char *)hostname, &dummy) &&
		    !pton6((const char *)hostname, tmp)) {
			rv = EAI_NONAME;
			goto out;
		}
		a.host = (const char *)hostname;
		if (sb_rpc(sb, (sbfn_t)srv_gai, &a, RPC_INTERRUPTIBLE,
		    NULL) < 0) {
			rv = EAI_AGAIN;
			goto out;
		}
	}
	for (k = 0; k < 2; k++) {
		struct dns_result *r = k ? a.r6 : a.r4;
		int fam = k ? AF_INET6 : AF_INET;

		if ((k ? a.rv6 : a.rv4) != 0)
			continue;
		for (i = 0; i < r->naddr; i++)
			for (j = 0; j < nst; j++) {
				int proto = protocol ? protocol :
				    socktypes[j] == SOCK_STREAM ? IPPROTO_TCP :
				    socktypes[j] == SOCK_DGRAM ? IPPROTO_UDP : 0;

				ai = ai_new(fam, socktypes[j], proto,
				    r->addr[i], port,
				    (flags & AI_CANONNAME) && head == NULL ?
				    r->cname : NULL);
				if (ai == NULL) {
					rv = EAI_MEMORY;
					goto out;
				}
				*tail = ai;
				tail = &ai->ai_next;
			}
	}
	if (head == NULL)
		rv = (a.rv4 == TRY_AGAIN || a.rv6 == TRY_AGAIN) ? EAI_AGAIN :
		    EAI_NONAME;
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

STRPTR
sb_gai_strerror(struct SocketBase *sb, LONG errnum)
{

	switch (errnum) {
	case 0: return (STRPTR)"Success";
	case EAI_AGAIN: return (STRPTR)"Temporary failure in name resolution";
	case EAI_BADFLAGS: return (STRPTR)"Invalid value for ai_flags";
	case EAI_FAIL: return (STRPTR)"Non-recoverable failure in name resolution";
	case EAI_FAMILY: return (STRPTR)"ai_family not supported";
	case EAI_MEMORY: return (STRPTR)"Memory allocation failure";
	case EAI_NONAME: return (STRPTR)"No address associated with hostname";
	case EAI_SERVICE: return (STRPTR)"Service not supported for socket type";
	case EAI_SOCKTYPE: return (STRPTR)"ai_socktype not supported";
	case EAI_SYSTEM: return (STRPTR)"System error";
	default: return (STRPTR)"Unknown error";
	}
}

LONG
sb_getnameinfo(struct SocketBase *sb, const struct sockaddr *sa, ULONG salen,
    STRPTR host, ULONG hostlen, STRPTR serv, ULONG servlen, ULONG flags)
{
	UWORD port;
	char buf[64];
	unsigned i;

	if (sa == NULL)
		return EAI_FAIL;
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;

		port = s4->sin_port;
		if (host && hostlen) {
			struct hostent *h = NULL;

			if (!(flags & NI_NUMERICHOST))
				h = sb_gethostbyaddr(sb,
				    (STRPTR)&s4->sin_addr, 4, AF_INET);
			if (h)
				sb_strlcpy((char *)host, h->h_name, hostlen);
			else if (flags & NI_NAMEREQD)
				return EAI_NONAME;
			else {
				fmt_ip4(buf, s4->sin_addr.s_addr);
				sb_strlcpy((char *)host, buf, hostlen);
			}
		}
	} else if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;

		port = s6->sin6_port;
		if (host && hostlen) {
			if (flags & NI_NAMEREQD)
				return EAI_NONAME;
			if (!sb_inet_ntop(sb, AF_INET6, (APTR)&s6->sin6_addr,
			    host, hostlen))
				return EAI_FAIL;
		}
	} else
		return EAI_FAMILY;

	if (serv && servlen) {
		const char *name = NULL;

		if (!(flags & NI_NUMERICSERV))
			for (i = 0; i < NSERVICES; i++)
				if (services[i].port == port &&
				    ((flags & NI_DGRAM) ? services[i].udp :
				    services[i].tcp)) {
					name = services[i].name;
					break;
				}
		if (name)
			sb_strlcpy((char *)serv, name, servlen);
		else {
			*fmt_dec(buf, port) = '\0';
			sb_strlcpy((char *)serv, buf, servlen);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * services / protocols / networks databases
 */

static struct servent *
servent_at(struct SocketBase *sb, unsigned i, int udp)
{

	sb_strlcpy(sb->sent_name, services[i].name, sizeof(sb->sent_name));
	sb->sent_aliases[0] = NULL;
	sb->sent.s_name = sb->sent_name;
	sb->sent.s_aliases = sb->sent_aliases;
	sb->sent.s_port = services[i].port;
	sb->sent.s_proto = udp ? "udp" : "tcp";
	return &sb->sent;
}

struct servent *
sb_getservbyname(struct SocketBase *sb, STRPTR name, STRPTR proto)
{
	int udp = proto && sb_strcmp((const char *)proto, "udp") == 0;
	unsigned i;

	for (i = 0; i < NSERVICES; i++)
		if (sb_strcmp(services[i].name, (const char *)name) == 0 &&
		    (proto == NULL || (udp ? services[i].udp : services[i].tcp)))
			return servent_at(sb, i, proto ? udp : !services[i].tcp);
	return NULL;
}

struct servent *
sb_getservbyport(struct SocketBase *sb, LONG port, STRPTR proto)
{
	int udp = proto && sb_strcmp((const char *)proto, "udp") == 0;
	unsigned i;

	for (i = 0; i < NSERVICES; i++)
		if (services[i].port == (UWORD)port &&
		    (proto == NULL || (udp ? services[i].udp : services[i].tcp)))
			return servent_at(sb, i, proto ? udp : !services[i].tcp);
	return NULL;
}

void
sb_setservent(struct SocketBase *sb, LONG stayopen)
{

	sb->dbpos_serv = 0;
}

void
sb_endservent(struct SocketBase *sb)
{

	sb->dbpos_serv = 0;
}

struct servent *
sb_getservent(struct SocketBase *sb)
{
	/* each service once per protocol: index = 2 * entry + udp */
	while ((unsigned)sb->dbpos_serv < 2 * NSERVICES) {
		unsigned i = sb->dbpos_serv / 2;
		int udp = sb->dbpos_serv & 1;

		sb->dbpos_serv++;
		if (udp ? services[i].udp : services[i].tcp)
			return servent_at(sb, i, udp);
	}
	return NULL;
}

static struct protoent *
protoent_at(struct SocketBase *sb, unsigned i)
{

	sb->pent_aliases[0] = NULL;
	sb->pent.p_name = (char *)protocols[i].name;
	sb->pent.p_aliases = sb->pent_aliases;
	sb->pent.p_proto = protocols[i].num;
	return &sb->pent;
}

struct protoent *
sb_getprotobyname(struct SocketBase *sb, STRPTR name)
{
	unsigned i;

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

/* networks database: none configured */
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

/* ------------------------------------------------------------------------
 * host identity
 */

LONG
sb_gethostname(struct SocketBase *sb, STRPTR name, LONG namelen)
{

	if (name == NULL || namelen <= 0) {
		sb_set_errno(sb, EINVAL);
		return -1;
	}
	sb_strlcpy((char *)name, hostname_cfg, namelen);
	return 0;
}

BOOL
sb_GetDefaultDomainName(struct SocketBase *sb, STRPTR buffer,
    LONG buffer_size)
{

	if (buffer == NULL || buffer_size <= 0)
		return FALSE;
	ns_lock();
	sb_strlcpy((char *)buffer, domainname, buffer_size);
	ReleaseSemaphore(&nslock);
	return TRUE;
}

void
sb_SetDefaultDomainName(struct SocketBase *sb, STRPTR buffer)
{

	netdb_set_domain((const char *)buffer);
}

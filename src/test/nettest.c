/*
 * Network test over a real SANA-II device: in WinUAE, uaenet.device unit 0
 * is SLIRP user-mode NAT (guest 10.0.2.15, gateway 10.0.2.2, which is the
 * emulator host's 127.0.0.1).  Brings up sana0, then
 *   1. TCP: connects to an echo server on the emulator host (10.0.2.2:7777,
 *      started by tools/run_emu.py --echo 7777) and checks the echo;
 *   2. UDP: asks the DNS server on the emulator host (10.0.2.2:53, started
 *      by tools/run_emu.py --dns 53) for amibsdnet.test, which it answers
 *      with 192.0.2.7: no Internet needed.
 *
 *   tools/link_test.sh src/test/nettest.c build/nettest
 *   python -I tools/run_emu.py build/nettest --net --echo 7777 --dns 53
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "rumpuser_amiga.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

int rump_init(void);
int rump___sysimpl_socket30(int, int, int);
int rump___sysimpl_connect(int, const void *, unsigned int);
long rump___sysimpl_sendto(int, const void *, unsigned long, int,
    const void *, unsigned int);
long rump___sysimpl_recvfrom(int, void *, unsigned long, int, void *,
    unsigned int *);
int rump___sysimpl_poll(void *, unsigned int, int);
int rump___sysimpl_close(int);

int rump_amibsdnet_ifcreate(const char *, const char *);
int rump_amibsdnet_ifaddr4(const char *, uint32_t, uint32_t);
int rump_amibsdnet_route4(int, uint32_t, uint32_t, uint32_t, uint32_t);

struct nb_sockaddr_in {
	uint8_t sin_len;
	uint8_t sin_family;
	uint16_t sin_port;
	uint32_t sin_addr;
	uint8_t sin_zero[8];
};
#define	NB_AF_INET	2
#define	NB_SOCK_STREAM	1
#define	NB_SOCK_DGRAM	2
#define	NB_RTM_ADD	1

/* NetBSD's struct pollfd and POLLIN (sys/sys/poll.h) */
struct nb_pollfd {
	int	fd;
	short	events;
	short	revents;
};
#define	NB_POLLIN	0x0001

#define	IP4(a, b, c, d)	(((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))

/* what tools/run_emu.py --dns answers */
#define	DNS_NAME	"amibsdnet.test"
#define	DNS_ADDR	IP4(192, 0, 2, 7)
#define	DNS_ID		0x4a42
#define	DNS_WAIT_MS	5000

extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

int _start(void);
static struct Task *parent;
static volatile int result = 20;

#define	P	amiga_rump_printf

static int
fail(const char *what)
{

	P("FAIL: %s (errno %d)\n", what, amiga_rump_errno());
	return 10;
}

static int
same(const void *a, const void *b, long n)
{
	const unsigned char *x = a, *y = b;

	while (n-- > 0)
		if (*x++ != *y++)
			return 0;
	return 1;
}

static void
sin_set(struct nb_sockaddr_in *sin, uint32_t addr, uint16_t port)
{

	memset(sin, 0, sizeof(*sin));
	sin->sin_len = sizeof(*sin);
	sin->sin_family = NB_AF_INET;
	sin->sin_port = port;
	sin->sin_addr = addr;
}

static int
tcp_echo_test(void)
{
	static const char msg[] = "AmiBSDNet: NetBSD TCP over SANA-II";
	struct nb_sockaddr_in sin;
	char buf[64];
	long n, got = 0;
	int s;

	sin_set(&sin, IP4(10, 0, 2, 2), 7777);
	if ((s = rump___sysimpl_socket30(NB_AF_INET, NB_SOCK_STREAM, 0)) < 0)
		return fail("tcp socket");
	if (rump___sysimpl_connect(s, &sin, sizeof(sin)) < 0)
		return fail("tcp connect 10.0.2.2:7777");
	P("tcp: connected to 10.0.2.2:7777\n");
	if (rump___sysimpl_sendto(s, msg, sizeof(msg), 0, NULL, 0) !=
	    (long)sizeof(msg))
		return fail("tcp send");
	while (got < (long)sizeof(msg)) {
		n = rump___sysimpl_recvfrom(s, buf + got, sizeof(buf) - got, 0,
		    NULL, NULL);
		if (n <= 0)
			return fail("tcp recv");
		got += n;
	}
	rump___sysimpl_close(s);
	if (got != (long)sizeof(msg) || !same(buf, msg, sizeof(msg))) {
		P("FAIL: tcp echo differs (%ld bytes)\n", got);
		return 10;
	}
	P("tcp: echo \"%s\"\n", buf);
	return 0;
}

/* skips the (possibly compressed) name at p; NULL if it runs past end */
static const unsigned char *
skip_name(const unsigned char *p, const unsigned char *end)
{

	while (p < end) {
		if (*p == 0)
			return p + 1;
		if ((*p & 0xc0) == 0xc0)
			return p + 2 <= end ? p + 2 : NULL;
		if (*p & 0xc0)
			return NULL;		/* reserved label types */
		p += *p + 1;
	}
	return NULL;
}

/* a DNS A query for name; checks the answer's ID and that it is addr */
static int
udp_dns_test(const char *name, uint32_t want)
{
	struct nb_sockaddr_in sin;
	struct nb_pollfd pfd;
	unsigned char q[512], *w;
	const unsigned char *p, *end;
	const char *label;
	unsigned int slen;
	long n;
	int s, i, an, qd, r;

	memset(q, 0, 12);
	q[0] = DNS_ID >> 8; q[1] = DNS_ID & 0xff;
	q[2] = 0x01;				/* recursion desired */
	q[5] = 1;				/* one question */
	w = q + 12;
	for (label = name; *label; ) {
		unsigned char *lenp = w++;

		for (i = 0; label[i] && label[i] != '.'; i++)
			*w++ = label[i];
		*lenp = (unsigned char)i;
		label += i;
		if (*label == '.')
			label++;
	}
	*w++ = 0;
	*w++ = 0; *w++ = 1;			/* type A */
	*w++ = 0; *w++ = 1;			/* class IN */

	if ((s = rump___sysimpl_socket30(NB_AF_INET, NB_SOCK_DGRAM, 0)) < 0)
		return fail("udp socket");
	sin_set(&sin, IP4(10, 0, 2, 2), 53);
	if (rump___sysimpl_sendto(s, q, w - q, 0, &sin, sizeof(sin)) !=
	    w - q) {
		rump___sysimpl_close(s);
		return fail("udp sendto 10.0.2.2:53");
	}
	/* no answer must not hang the test */
	pfd.fd = s;
	pfd.events = NB_POLLIN;
	pfd.revents = 0;
	if ((r = rump___sysimpl_poll(&pfd, 1, DNS_WAIT_MS)) <= 0) {
		rump___sysimpl_close(s);
		if (r == 0) {
			P("FAIL: no DNS answer from 10.0.2.2:53 within %d ms "
			    "(tools/run_emu.py --dns 53)\n", DNS_WAIT_MS);
			return 10;
		}
		return fail("udp poll");
	}
	slen = sizeof(sin);
	n = rump___sysimpl_recvfrom(s, q, sizeof(q), 0, &sin, &slen);
	rump___sysimpl_close(s);
	if (n < 12)
		return fail("udp recvfrom");
	if (((q[0] << 8) | q[1]) != DNS_ID || !(q[2] & 0x80)) {
		P("FAIL: not the answer to our DNS query (id %04x)\n",
		    (q[0] << 8) | q[1]);
		return 10;
	}

	end = q + n;
	qd = (q[4] << 8) | q[5];
	an = (q[6] << 8) | q[7];
	p = q + 12;
	while (qd-- > 0) {			/* skip questions */
		if ((p = skip_name(p, end)) == NULL || p + 4 > end)
			goto bad;
		p += 4;
	}
	while (an-- > 0) {
		int type, rdlen;

		if ((p = skip_name(p, end)) == NULL || p + 10 > end)
			goto bad;
		type = (p[0] << 8) | p[1];
		rdlen = (p[8] << 8) | p[9];
		p += 10;
		if (p + rdlen > end)
			goto bad;
		if (type == 1 && rdlen == 4) {
			uint32_t got = ((uint32_t)p[0] << 24) | (p[1] << 16) |
			    (p[2] << 8) | p[3];

			P("udp: DNS %s = %u.%u.%u.%u\n", name, p[0], p[1], p[2],
			    p[3]);
			if (got != want) {
				P("FAIL: expected %u.%u.%u.%u\n",
				    (unsigned)(want >> 24),
				    (unsigned)(want >> 16) & 255,
				    (unsigned)(want >> 8) & 255,
				    (unsigned)want & 255);
				return 10;
			}
			return 0;
		}
		p += rdlen;
	}
	P("FAIL: no A record for %s (rcode %d)\n", name, q[3] & 15);
	return 10;
bad:
	P("FAIL: DNS answer cut short or malformed (%ld bytes)\n", n);
	return 10;
}

static void
test_proc(void)
{
	int rv;

	crash_install();
	if (amiga_rump_hostinit(0) != 0) {
		result = 30;
		goto out;
	}
	amiga_rump_notifytask = parent;
	if ((rv = rump_init()) != 0) {
		P("FAIL: rump_init returned %d\n", rv);
		goto out;
	}
	if (rump_amibsdnet_ifcreate("sana0", "uaenet.device:0") != 0) {
		result = fail("create sana0 on uaenet.device:0");
		goto out;
	}
	if (rump_amibsdnet_ifaddr4("sana0", IP4(10, 0, 2, 15),
	    IP4(255, 255, 255, 0)) != 0) {
		result = fail("address 10.0.2.15/24");
		goto out;
	}
	if (rump_amibsdnet_route4(NB_RTM_ADD, 0, 0, IP4(10, 0, 2, 2), 0) != 0) {
		result = fail("default route via 10.0.2.2");
		goto out;
	}
	P("sana0: 10.0.2.15/24, default route 10.0.2.2\n");

	result = tcp_echo_test();
	if (result == 0)
		result = udp_dns_test(DNS_NAME, DNS_ADDR);
	P(result == 0 ? "PASS\n" : "test failed\n");
out:
	Forbid();
	Signal(parent, SIGBREAKF_CTRL_F);
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	BPTR f, log;
	ULONG sigs;

	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	if (DOSBase == NULL)
		return 20;
	parent = SysBase->ThisTask;

	amiga_rump_debug = 1;
	log = Open((CONST_STRPTR)"DH0:rump.log", MODE_NEWFILE);
	amiga_rump_loginit((long)(log ? log : Output()));
	crash_install();

	for (void (**c)(void) = __init_array_start; c < __init_array_end; c++)
		(*c)();

	if (CreateNewProcTags(NP_Entry, (ULONG)test_proc,
	    NP_Name, (ULONG)"nettest", NP_StackSize, 256 * 1024,
	    TAG_DONE) == NULL)
		return 20;

	sigs = Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);
	if (sigs & SIGBREAKF_CTRL_C)
		P("rump kernel exited (code %d)\n", amiga_rump_exitcode);
	Delay(10);
	if ((f = Open((CONST_STRPTR)"DH0:done", MODE_NEWFILE))) {
		Write(f, result == 0 ? "PASS\n" : "FAIL\n", 5);
		Close(f);
	}
	for (;;)
		Wait(0x80000000UL);
}

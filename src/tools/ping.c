/*
 * Ping: send ICMP echo requests through bsdsocket.library.
 *
 *   Ping HOST [COUNT=<n>]
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <amibsdnet/net.h>
#include <bsdsocket_inline.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *SocketBase;
struct Device *TimerBase;

static const char verstag[] __attribute__((used)) =
    "\0$VER: Ping 0.8.2 (10.10.2026)";

#define	SOCK_RAW	3
#define	IPPROTO_ICMP	1
#define	ICMP_ECHO	8
#define	ICMP_ECHOREPLY	0
#define	DATALEN		56

static LONG errno_;

void	*memset(void *, int, unsigned long);

static void
putn(ULONG v)
{
	char b[12];
	int i = 11;

	b[i] = '\0';
	do {
		b[--i] = '0' + v % 10;
		v /= 10;
	} while (v);
	PutStr((CONST_STRPTR)(b + i));
}

static UWORD
cksum(const UBYTE *p, int len)
{
	ULONG s = 0;

	for (; len > 1; p += 2, len -= 2)
		s += (p[0] << 8) | p[1];
	if (len)
		s += p[0] << 8;
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return (UWORD)~s;
}

/*
 * EClock ticks (low 32 bits; wraps after an hour or more, plenty for ping)
 * and conversion of tick differences without 64-bit arithmetic.
 */
static ULONG
now_ticks(void)
{
	struct EClockVal ev;

	ReadEClock(&ev);
	return ev.ev_lo;
}

/* tick difference -> units of 0.1 ms */
static ULONG
ticks_to_tenth_ms(ULONG d, ULONG freq)
{

	ULONG r = d % freq, ms, rem;

	/* exact, without 64-bit arithmetic: r * 1000 fits (freq < 4 MHz),
	   and so does the remainder * 10 */
	ms = r * 1000 / freq;
	rem = r * 1000 % freq;
	return (d / freq) * 10000 + ms * 10 + rem * 10 / freq;
}

/* tick difference -> microseconds (for WaitSelect timeouts) */
static ULONG
ticks_to_us(ULONG d, ULONG freq)
{

	return ticks_to_tenth_ms(d, freq) * 100;
}

static int
run(const char *host, ULONG count)
{
	struct sockaddr_in to, from;
	struct hostent *h;
	UBYTE pkt[8 + DATALEN], buf[1500];
	ULONG freq, t0, sent = 0, recvd = 0, rttsum = 0, seq;
	struct EClockVal ev;
	LONG s, n;
	socklen_t flen;
	UWORD id = (UWORD)(ULONG)SysBase->ThisTask;
	ami_fd_set rfds;
	struct __timeval tv;

	freq = ReadEClock(&ev);
	if ((h = gethostbyname((STRPTR)host)) == NULL) {
		PutStr((CONST_STRPTR)"Ping: unknown host ");
		PutStr((CONST_STRPTR)host);
		PutStr((CONST_STRPTR)"\n");
		return RETURN_ERROR;
	}
	memset(&to, 0, sizeof(to));
	to.sin_len = sizeof(to);
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = *(ULONG *)h->h_addr_list[0];
	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) < 0) {
		PutStr((CONST_STRPTR)"Ping: cannot open raw socket\n");
		return RETURN_ERROR;
	}
	PutStr((CONST_STRPTR)"PING ");
	PutStr((CONST_STRPTR)host);
	PutStr((CONST_STRPTR)" (");
	PutStr((CONST_STRPTR)Inet_NtoA(to.sin_addr.s_addr));
	PutStr((CONST_STRPTR)"): 56 data bytes\n");

	for (seq = 0; seq < count; seq++) {
		ULONG deadline;
		int i;

		memset(pkt, 0, sizeof(pkt));
		pkt[0] = ICMP_ECHO;
		pkt[4] = id >> 8; pkt[5] = id;
		pkt[6] = seq >> 8; pkt[7] = seq;
		for (i = 8; i < (int)sizeof(pkt); i++)
			pkt[i] = i;
		t0 = now_ticks();
		{
			UWORD c = cksum(pkt, sizeof(pkt));

			pkt[2] = c >> 8;
			pkt[3] = c;
		}
		if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&to,
		    sizeof(to)) < 0) {
			PutStr((CONST_STRPTR)"Ping: send failed\n");
			break;
		}
		sent++;
		deadline = t0 + freq;		/* one second */
		for (;;) {
			ULONG t = now_ticks();

			if ((LONG)(deadline - t) <= 0) {
				PutStr((CONST_STRPTR)"Request timeout for icmp_seq ");
				putn(seq);
				PutStr((CONST_STRPTR)"\n");
				break;
			}
			AMI_FD_ZERO(&rfds);
			AMI_FD_SET(s, &rfds);
			{
				ULONG us = ticks_to_us(deadline - t, freq);

				tv.tv_secs = us / 1000000;
				tv.tv_micro = us % 1000000;
			}
			n = WaitSelect(s + 1, &rfds, NULL, NULL, &tv, NULL);
			if (n < 0)
				goto done;	/* Ctrl-C */
			if (n == 0)
				continue;
			flen = sizeof(from);
			n = recvfrom(s, buf, sizeof(buf), 0,
			    (struct sockaddr *)&from, &flen);
			if (n < 20)
				continue;
			{
				int hl = (buf[0] & 15) * 4;
				UBYTE *ic = buf + hl;

				if (n < hl + 8 || ic[0] != ICMP_ECHOREPLY ||
				    ((ic[4] << 8) | ic[5]) != id ||
				    ((ic[6] << 8) | ic[7]) != (UWORD)seq)
					continue;
				t = ticks_to_tenth_ms(now_ticks() - t0, freq);
				recvd++;
				rttsum += t;
				putn(n - hl);
				PutStr((CONST_STRPTR)" bytes from ");
				PutStr((CONST_STRPTR)Inet_NtoA(from.sin_addr.s_addr));
				PutStr((CONST_STRPTR)": icmp_seq=");
				putn(seq);
				PutStr((CONST_STRPTR)" ttl=");
				putn(buf[8]);
				PutStr((CONST_STRPTR)" time=");
				putn(t / 10);
				PutStr((CONST_STRPTR)".");
				putn(t % 10);
				PutStr((CONST_STRPTR)" ms\n");
			}
			/* pace: one request per second */
			while ((LONG)(deadline - now_ticks()) > 0 &&
			    seq + 1 < count)
				Delay(2);
			break;
		}
	}
done:
	CloseSocket(s);
	PutStr((CONST_STRPTR)"--- ping statistics ---\n");
	putn(sent);
	PutStr((CONST_STRPTR)" packets transmitted, ");
	putn(recvd);
	PutStr((CONST_STRPTR)" packets received");
	if (recvd) {
		PutStr((CONST_STRPTR)", average ");
		putn(rttsum / recvd / 10);
		PutStr((CONST_STRPTR)" ms");
	}
	PutStr((CONST_STRPTR)"\n");
	return recvd ? RETURN_OK : RETURN_WARN;
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	struct RDArgs *rda;
	static struct timerequest tr;	/* (zeroed) */
	LONG arg[2] = { 0, 0 };
	int rc = RETURN_FAIL;

	SysBase = *(struct ExecBase **)4;
	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37)) == NULL)
		return RETURN_FAIL;
	if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK,
	    (struct IORequest *)&tr, 0) != 0)
		goto out;
	TimerBase = tr.tr_node.io_Device;
	if ((rda = ReadArgs((CONST_STRPTR)"HOST/A,COUNT/K/N", arg, NULL)) == NULL) {
		PrintFault(IoErr(), (CONST_STRPTR)"Ping");
		goto close;
	}
	if ((SocketBase = OpenLibrary("bsdsocket.library", 4)) == NULL) {
		PutStr((CONST_STRPTR)"Ping: no TCP/IP stack running\n");
	} else {
		SetErrnoPtr(&errno_, sizeof(errno_));
		LONG count = arg[1] ? *(LONG *)arg[1] : 4;

		/* (at least one; a negative one is not "forever") */
		rc = run((const char *)arg[0], count > 0 ? count : 1);
		CloseLibrary(SocketBase);
	}
	FreeArgs(rda);
close:
	CloseDevice((struct IORequest *)&tr);
out:
	CloseLibrary((struct Library *)DOSBase);
	return rc;
}

void *
memset(void *d, int c, unsigned long n)
{
	char *p = d;

	while (n--)
		*p++ = c;
	return d;
}

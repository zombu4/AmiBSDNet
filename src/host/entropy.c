/*
 * Host entropy pool behind rumpuser_getrandom().
 *
 * The rump kernel's only entropy source is the hypercall: hyperentropy.c
 * (netbsd-src/sys/rump/librump/rumpkern/hyperentropy.c:41-60) asks for
 * RUMPUSER_RANDOM_HARD data and credits every byte it gets as 8 bits
 * (rnd_add_data_sync(..., NBBY*n)).  "HARD" is "true randomness instead
 * of something from a PRNG" (rumpuser.3, "Random pool").  A classic
 * Amiga has no random number generator (none is exposed by Emu68
 * either), so the randomness comes from the timing of real hardware
 * events: the E-Clock (timer.doc ReadEClock: the 64-bit E-Clock tick
 * count, callable from interrupts) is read whenever a SANA-II request
 * comes back from the driver, i.e. when a frame arrived from the
 * network, a frame left, or the link changed.
 *
 * Mixing: every sample goes into a running SHA-256 hash (sha256.c).
 * Output is taken by finishing a copy of that hash into D, returning
 * SHA-256(0x01 || D) and restarting the pool as SHA-256(0x00 || D ...):
 * the output and the next pool state are independent hashes of D, so
 * output says nothing about later output, and a pool state seen later
 * says nothing about output given before.
 *
 * Accounting: a sample counts only if it is credited by its caller and
 * NetBSD's own timing test accepts it (first, second and third order
 * differences of the time all nonzero: rnd_delta_estimate() in
 * netbsd-src/sys/kern/kern_entropy.c:2019-2046, kept per source like its
 * rc_timedelta), and then for one bit only.  The pool holds at most 256
 * credited bits (one SHA-256 state).  Nothing is given out as HARD before
 * 256 bits have been collected once; after that, every 8 credited bits
 * allow one byte.
 *
 * Today no caller credits anything: the only events are network ones,
 * and NetBSD does not count network timing as entropy ("an adversary can
 * influence network packet timings", RND_FLAG_NO_COLLECT for
 * RND_TYPE_NET, kern_entropy.c:1805-1811).  So HARD requests get EAGAIN
 * and the kernel's entropy pool stays unseeded: its random numbers (TCP
 * sequence numbers, ports, DNS ids) come from its generator without a
 * secret seed.  A source that is not network timing is needed to change
 * that.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include "entropy.h"
#include "sha256.h"

extern struct ExecBase *SysBase;
extern struct Device *TimerBase;	/* rumpuser_amiga.c */

#define	POOL_BITS	256		/* SHA-256 state */
#define	SEED_BITS	256		/* before the first HARD output */

struct delta {
	ULONG	x, dx, d2x;
};

static struct sha256 pool;
static int pool_ready;
static unsigned pool_bits;		/* credited, not yet given out */
static int seeded;			/* SEED_BITS were reached once */
static struct delta deltas[ENT_NSRC];
static ULONG nsamples;

struct entwaiter {
	struct MinNode node;
	struct Task *task;
	ULONG	mask;
};
static struct MinList waiters = {
	(struct MinNode *)&waiters.mlh_Tail, NULL,
	(struct MinNode *)&waiters.mlh_Head
};

static ULONG
absdiff(ULONG a, ULONG b)
{

	return a > b ? a - b : b - a;
}

/* rnd_delta_estimate() / rnd_dt_estimate(), kern_entropy.c:2019-2075 */
static int
delta_ok(struct delta *d, ULONG t)
{
	ULONG delta, delta2, delta3;
	LONG sd;

	/* (as rnd_dt_estimate() has it, kern_entropy.c:2060-2068: a signed
	   32-bit delta, UINT32_MAX - x + t or x - t, then its absolute
	   value) */
	sd = t < d->x ? (LONG)(0xffffffffUL - d->x + t) : (LONG)(d->x - t);
	if (sd < 0)
		sd = -sd;
	delta = (ULONG)sd;
	delta2 = absdiff(d->dx, delta);
	delta3 = absdiff(d->d2x, delta2);
	d->x = t;
	d->dx = delta;
	d->d2x = delta2;
	return delta != 0 && delta2 != 0 && delta3 != 0;
}

static int
hard_available(void)
{

	return seeded && pool_bits >= 8;
}

/* under Forbid() */
static void
pool_start(void)
{

	if (!pool_ready) {
		sha256_init(&pool);
		pool_ready = 1;
	}
}

/* under Forbid() */
static void
wake_waiters(void)
{
	struct entwaiter *w;

	if (!hard_available())
		return;
	while ((w = (struct entwaiter *)RemHead((struct List *)&waiters)))
		Signal(w->task, w->mask);
}

void
amiga_entropy_init(void)
{
	struct {
		struct EClockVal ev;
		struct timeval tv;
		ULONG	idle, disp, fast, chip;
		APTR	task;
	} s;

	Forbid();
	pool_start();
	Permit();
	if (TimerBase == NULL)
		return;
	/* mixed in, never credited: the boot time and the memory state are
	   not secret */
	ReadEClock(&s.ev);
	GetSysTime(&s.tv);
	s.idle = SysBase->IdleCount;
	s.disp = SysBase->DispCount;
	s.fast = AvailMem(MEMF_FAST);
	s.chip = AvailMem(MEMF_CHIP);
	s.task = SysBase->ThisTask;
	Forbid();
	sha256_update(&pool, &s, sizeof(s));
	Permit();
}

void
amiga_entropy_event(unsigned src, int credit, const void *data, size_t len)
{
	struct {
		struct EClockVal ev;
		ULONG	src, n;
	} s;

	if (TimerBase == NULL || src >= ENT_NSRC)
		return;
	ReadEClock(&s.ev);
	s.src = src;
	Forbid();
	pool_start();
	s.n = ++nsamples;
	sha256_update(&pool, &s, sizeof(s));
	if (data && len)
		sha256_update(&pool, data, len);
	if (delta_ok(&deltas[src], s.ev.ev_lo) && credit &&
	    src != ENT_SRC_HOST) {
		if (pool_bits < POOL_BITS)
			pool_bits++;
		if (pool_bits >= SEED_BITS)
			seeded = 1;
		wake_waiters();
	}
	Permit();
}

size_t
amiga_entropy_extract(void *buf, size_t len, int hard)
{
	static const UBYTE out_tag = 0x01, pool_tag = 0x00;
	struct sha256 c;
	UBYTE d[SHA256_DIGEST_LEN], o[SHA256_DIGEST_LEN];
	UBYTE *p = buf;
	size_t n, i;

	if (len > SHA256_DIGEST_LEN)
		len = SHA256_DIGEST_LEN;
	Forbid();
	pool_start();
	if (hard) {
		n = hard_available() ? pool_bits / 8 : 0;
		if (n > len)
			n = len;
	} else
		n = len;
	if (n == 0) {
		Permit();
		return 0;
	}
	c = pool;
	sha256_final(&c, d);
	sha256_init(&c);
	sha256_update(&c, &out_tag, 1);
	sha256_update(&c, d, sizeof(d));
	sha256_final(&c, o);
	sha256_init(&pool);
	sha256_update(&pool, &pool_tag, 1);
	sha256_update(&pool, d, sizeof(d));
	if (hard)
		pool_bits -= 8 * n;
	Permit();
	for (i = 0; i < n; i++)
		p[i] = o[i];
	for (i = 0; i < sizeof(d); i++) {
		((volatile UBYTE *)d)[i] = 0;
		((volatile UBYTE *)o)[i] = 0;
	}
	return n;
}

void
amiga_entropy_wait(unsigned long mask)
{
	struct entwaiter w;
	struct MinNode *n;

	w.task = SysBase->ThisTask;
	w.mask = mask;
	Forbid();
	while (!hard_available()) {
		SetSignal(0, mask);
		AddTail((struct List *)&waiters, (struct Node *)&w.node);
		Wait(mask);		/* (breaks the Forbid while asleep) */
		/* wake_waiters() removed it; any other use of the same
		   signal bit left it listed */
		for (n = waiters.mlh_Head; n->mln_Succ; n = n->mln_Succ)
			if (n == &w.node) {
				Remove((struct Node *)&w.node);
				break;
			}
	}
	Permit();
}

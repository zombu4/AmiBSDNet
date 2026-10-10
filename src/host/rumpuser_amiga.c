/*
 * rumpuser(3) hypercalls for AmigaOS (Exec/DOS), RUMPUSER_VERSION 17.
 *
 * Model: lib/librumpuser/rumpuser_pth.c, with pthreads replaced by Exec.
 *
 *  - rump threads are DOS processes; per-thread state hangs off
 *    tc_UserData (struct amthread).  RUMP_CURLWP_HYPERCALL means curlwp
 *    is fetched through rumpuser_curlwp(), so no TLS is needed.
 *  - mutexes and rwlocks are SignalSemaphores plus the owner/reader
 *    bookkeeping rump expects.
 *  - condition variables are waiter lists manipulated under Forbid(),
 *    woken with a per-thread signal bit.  Waiting inside Forbid() is the
 *    standard Exec idiom: Wait() breaks the Forbid while asleep, so the
 *    "release mutex, sleep" step cannot lose a wakeup.
 *  - timeouts and sleeps use a per-thread timer.device request.
 *
 * Every blocking host operation is bracketed by KLOCK_WRAP(), which gives
 * the rump virtual CPU back while the host thread sleeps.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "rumpuser_amiga.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
struct Device *TimerBase;

/* seconds from the AmigaOS epoch (1978-01-01) to the Unix epoch */
#define	AMIGA_EPOCH_OFFSET	252460800UL

#define	THREAD_STACK		(32 * 1024)	/* NetBSD/m68k uses 16 KB */

struct Task *amiga_rump_notifytask;
int amiga_rump_debug;
volatile int amiga_rump_exitcode;
volatile int amiga_rump_exited;

static struct rumpuser_hyperup hyp;

/* amiga_rump_debug >= 2: log every hypercall on entry */
#define	TRACE(fmt, ...)							\
do {									\
	if (amiga_rump_debug > 1)					\
		amiga_rump_printf("hc %s: " fmt, __func__, ##__VA_ARGS__); \
} while (0)
#define	TRACE0()	TRACE("\n")

static struct Task *hosttask;
static BPTR logfh;
static struct SignalSemaphore logsem;
static ULONG eclock_freq;

/* ------------------------------------------------------------------------
 * per-thread state
 */

struct amthread {
	struct lwp *curlwp;
	int err;

	BYTE cvsig;			/* -1 until allocated */
	ULONG cvmask;

	struct MsgPort *tport;		/* lazily opened timer */
	struct timerequest *treq;

	/* creation / join */
	void *(*func)(void *);
	void *arg;
	int joinable;
	volatile int done;
	struct Task *joiner;
	ULONG joinmask;

	APTR saved_userdata;		/* for adopted (non-rump) tasks */
	void *jmpbuf[5];		/* __builtin_setjmp buffer */
};

static struct amthread *
self(void)
{

	return (struct amthread *)SysBase->ThisTask->tc_UserData;
}

static struct amthread *
thread_alloc(void)
{
	struct amthread *t;

	t = AllocVec(sizeof(*t), MEMF_FAST | MEMF_CLEAR);
	if (t)
		t->cvsig = -1;
	return t;
}

/* called on the thread itself: signal bits belong to the calling task */
static int
thread_setup_self(struct amthread *t)
{

	t->cvsig = AllocSignal(-1);
	if (t->cvsig == -1)
		return RUMPUSER_EAGAIN;
	t->cvmask = 1UL << t->cvsig;
	return 0;
}

static void
thread_teardown_self(struct amthread *t)
{

	if (t->treq) {
		CloseDevice((struct IORequest *)t->treq);
		DeleteIORequest((struct IORequest *)t->treq);
		t->treq = NULL;
	}
	if (t->tport) {
		DeleteMsgPort(t->tport);
		t->tport = NULL;
	}
	if (t->cvsig != -1) {
		FreeSignal(t->cvsig);
		t->cvsig = -1;
	}
}

static struct timerequest *
thread_timer(struct amthread *t)
{

	if (t->treq)
		return t->treq;
	if ((t->tport = CreateMsgPort()) == NULL)
		return NULL;
	t->treq = (struct timerequest *)CreateIORequest(t->tport,
	    sizeof(struct timerequest));
	if (t->treq == NULL ||
	    OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
	    (struct IORequest *)t->treq, 0) != 0) {
		if (t->treq)
			DeleteIORequest((struct IORequest *)t->treq);
		t->treq = NULL;
		DeleteMsgPort(t->tport);
		t->tport = NULL;
		return NULL;
	}
	return t->treq;
}

/* ------------------------------------------------------------------------
 * scheduling helpers (see rumpuser_int.h)
 */

static inline void
rumpkern_unsched(int *nlocks, void *interlock)
{

	hyp.hyp_backend_unschedule(0, nlocks, interlock);
}

static inline void
rumpkern_sched(int nlocks, void *interlock)
{

	hyp.hyp_backend_schedule(nlocks, interlock);
}

#define	KLOCK_WRAP(a)							\
do {									\
	int nlocks_;							\
	rumpkern_unsched(&nlocks_, NULL);				\
	a;								\
	rumpkern_sched(nlocks_, NULL);					\
} while (0)

/* ------------------------------------------------------------------------
 * small libc replacements for host code (no C library is linked)
 */

void *
memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;

	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

void *
memcpy(void *dst, const void *src, size_t n)
{

	CopyMem((APTR)src, dst, n);
	return dst;
}

static size_t
host_strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return p - s;
}

static int
host_strcmp(const char *a, const char *b)
{

	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

/* ------------------------------------------------------------------------
 * console
 */

static char logbuf[256];
static size_t loglen;

static void
log_flush_locked(void)
{

	if (loglen && logfh)
		Write(logfh, logbuf, loglen);
	loglen = 0;
}

static void
log_putc_locked(int c)
{

	logbuf[loglen++] = (char)c;
	if (c == '\n' || loglen == sizeof(logbuf) || amiga_rump_debug)
		log_flush_locked();
}

void
rumpuser_putchar(int c)
{

	ObtainSemaphore(&logsem);
	log_putc_locked(c);
	ReleaseSemaphore(&logsem);
}

static void
fmt_num(unsigned long long v, int base, int neg, int width, char pad)
{
	char tmp[24];
	int i = 0;

	do {
		tmp[i++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	if (neg)
		tmp[i++] = '-';
	while (i < width--)
		log_putc_locked(pad);
	while (i)
		log_putc_locked(tmp[--i]);
}

/* %s %c %d %i %u %x %p %% with l/ll/z modifiers, '0' and width */
void
amiga_rump_vprintf(const char *fmt, va_list ap)
{
	ObtainSemaphore(&logsem);
	for (; *fmt; fmt++) {
		int lng = 0, width = 0;
		char pad = ' ';
		unsigned long long u;
		long long s;
		const char *str;

		if (*fmt != '%') {
			log_putc_locked(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == '0')
			pad = '0', fmt++;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l' || *fmt == 'z')
			lng += (*fmt++ == 'l') ? 1 : 1;
		switch (*fmt) {
		case 'd':
		case 'i':
			s = lng >= 2 ? va_arg(ap, long long) :
			    lng ? va_arg(ap, long) : va_arg(ap, int);
			fmt_num(s < 0 ? -(unsigned long long)s : s, 10, s < 0,
			    width, pad);
			break;
		case 'u':
		case 'x':
			u = lng >= 2 ? va_arg(ap, unsigned long long) :
			    lng ? va_arg(ap, unsigned long) :
			    va_arg(ap, unsigned int);
			fmt_num(u, *fmt == 'u' ? 10 : 16, 0, width, pad);
			break;
		case 'p':
			log_putc_locked('0');
			log_putc_locked('x');
			fmt_num((unsigned long)va_arg(ap, void *), 16, 0, 8, '0');
			break;
		case 'c':
			log_putc_locked(va_arg(ap, int));
			break;
		case 's':
			str = va_arg(ap, const char *);
			if (str == NULL)
				str = "(null)";
			while (*str)
				log_putc_locked(*str++);
			break;
		case '%':
			log_putc_locked('%');
			break;
		default:
			log_putc_locked('%');
			if (*fmt)
				log_putc_locked(*fmt);
			else
				fmt--;
			break;
		}
	}
	ReleaseSemaphore(&logsem);
}

/*
 * Format with Amiga-style arguments: a packed array of LONGs (vsyslog()
 * convention).  %s %c %d %i %u %x %X %o %p %%, with the flags - 0 + space
 * #, a width and a precision (also '*') and the h/l/ll/z modifiers, so
 * that every conversion takes exactly its arguments: a format the
 * formatter did not understand must not shift the later ones (a %s would
 * then read an integer as a pointer).
 */
#define	NEXTARG()	(args ? *args++ : 0)

static void
pad_out(int n, char c)
{

	while (n-- > 0)
		log_putc_locked(c);
}

void
amiga_rump_vprintf_longs(const char *fmt, const LONG *args)
{
	ObtainSemaphore(&logsem);
	for (; *fmt; fmt++) {
		const char *str;
		char tmp[16];
		int left = 0, zero = 0, width = 0, prec = -1, len, i;
		ULONG u;
		LONG v;

		if (*fmt != '%') {
			log_putc_locked(*fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				zero = 1;
			else if (*fmt != '+' && *fmt != ' ' && *fmt != '#')
				break;
		}
		if (*fmt == '*') {
			width = (int)NEXTARG();
			if (width < 0)
				left = 1, width = -width;
			fmt++;
		} else
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = (int)NEXTARG();
				fmt++;
			} else
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
		}
		while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z')
			fmt++;
		if (width > 200)
			width = 200;
		switch (*fmt) {
		case 'd':
		case 'i':
		case 'u':
		case 'x':
		case 'X':
		case 'o':
		case 'p':
			v = NEXTARG();
			if ((*fmt == 'd' || *fmt == 'i') && v < 0) {
				u = -(ULONG)v;
				tmp[0] = '-';
				len = 1;
			} else {
				u = (ULONG)v;
				len = 0;
			}
			{
				int base = *fmt == 'o' ? 8 : (*fmt == 'x' ||
				    *fmt == 'X' || *fmt == 'p') ? 16 : 10;
				char digits[12];
				int nd = 0;

				do {
					digits[nd++] = "0123456789abcdef"[u % base];
					u /= base;
				} while (u);
				while (nd)
					tmp[len++] = digits[--nd];
			}
			if (!left && !zero)
				pad_out(width - len, ' ');
			i = 0;
			if (!left && zero) {
				if (tmp[0] == '-')
					log_putc_locked(tmp[i++]);
				pad_out(width - len, '0');
			}
			for (; i < len; i++)
				log_putc_locked(tmp[i]);
			if (left)
				pad_out(width - len, ' ');
			break;
		case 'c':
			if (!left)
				pad_out(width - 1, ' ');
			log_putc_locked((int)NEXTARG());
			if (left)
				pad_out(width - 1, ' ');
			break;
		case 's':
			str = (const char *)NEXTARG();
			if (str == NULL)
				str = "(null)";
			for (len = 0; str[len] && (prec < 0 || len < prec); len++)
				;
			if (!left)
				pad_out(width - len, ' ');
			for (i = 0; i < len; i++)
				log_putc_locked(str[i]);
			if (left)
				pad_out(width - len, ' ');
			break;
		case '%':
			log_putc_locked('%');
			break;
		case '\0':
			fmt--;
			break;
		default:
			/* unknown: printed, and like any conversion it takes
			   one argument */
			log_putc_locked('%');
			log_putc_locked(*fmt);
			(void)NEXTARG();
			break;
		}
	}
	ReleaseSemaphore(&logsem);
}
#undef	NEXTARG

void
amiga_rump_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	amiga_rump_vprintf(fmt, ap);
	va_end(ap);
}

void
rumpuser_dprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	amiga_rump_vprintf(fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------------
 * init
 */

static int loginited;

void
amiga_rump_loginit(long log)
{

	if (!loginited) {
		InitSemaphore(&logsem);
		loginited = 1;
	}
	logfh = (BPTR)log;
}

int
amiga_rump_hostinit(long log)
{
	struct amthread *t;
	struct EClockVal ev;

	if (log)
		amiga_rump_loginit(log);
	hosttask = SysBase->ThisTask;

	if ((t = thread_alloc()) == NULL)
		return RUMPUSER_ENOMEM;
	t->saved_userdata = hosttask->tc_UserData;
	hosttask->tc_UserData = t;
	if (thread_setup_self(t) != 0 || thread_timer(t) == NULL)
		return RUMPUSER_EAGAIN;
	TimerBase = t->treq->tr_node.io_Device;
	eclock_freq = ReadEClock(&ev);
	return 0;
}

int
amiga_rump_errno(void)
{

	return self()->err;
}

int
rumpuser_init(int version, const struct rumpuser_hyperup *hyperup)
{

	TRACE0();
	if (version != RUMPUSER_VERSION) {
		amiga_rump_printf("rumpuser: unsupported hypercall version %d "
		    "(this host implements %d)\n", version, RUMPUSER_VERSION);
		return 1;
	}
	if (TimerBase == NULL) {
		amiga_rump_printf("rumpuser: amiga_rump_hostinit() not called\n");
		return 1;
	}
	hyp = *hyperup;
	if (amiga_rump_debug)
		amiga_rump_printf("rumpuser: init ok, version %d\n", version);
	return 0;
}

/* ------------------------------------------------------------------------
 * memory
 *
 * All stack memory comes from Fast RAM (MEMF_FAST); Chip RAM is left to
 * the custom chips.
 *
 * Blocks come straight from AllocMem() and go back with FreeMem() using
 * the length rump passes to rumpuser_free() (always the allocation
 * length).  Aligned requests (the kernel asks for whole 8 KB pages) use
 * the classic Amiga technique: allocate size + alignment, then FreeMem()
 * the unused head and tail, so no memory is wasted on alignment.
 */

#define	MEM_ROUND(n)	(((n) + 7) & ~(size_t)7)	/* MEM_BLOCKSIZE */

int
rumpuser_malloc(size_t len, int alignment, void **memp)
{
	size_t size = MEM_ROUND(len ? len : 1), total, head, tail;
	UBYTE *raw, *a;

	TRACE0();
	if (alignment <= 8) {
		if ((raw = AllocMem(size, MEMF_FAST)) == NULL)
			return RUMPUSER_ENOMEM;
		*memp = raw;
		return 0;
	}
	total = size + alignment;
	if ((raw = AllocMem(total, MEMF_FAST)) == NULL)
		return RUMPUSER_ENOMEM;
	a = (UBYTE *)(((uintptr_t)raw + alignment - 1) &
	    ~(uintptr_t)(alignment - 1));
	head = a - raw;
	tail = total - head - size;
	if (head)
		FreeMem(raw, head);
	if (tail)
		FreeMem(a + size, tail);
	*memp = a;
	return 0;
}

void
rumpuser_free(void *mem, size_t len)
{

	TRACE0();
	if (mem)
		FreeMem(mem, MEM_ROUND(len ? len : 1));
}

int
rumpuser_anonmmap(void *prefaddr, size_t size, int alignbit, int exec,
    void **memp)
{

	TRACE0();
	return rumpuser_malloc(size, 1 << alignbit, memp);
}

void
rumpuser_unmap(void *addr, size_t len)
{

	TRACE0();
	rumpuser_free(addr, len);
}

unsigned long
rumpuser_getpagesize(void)
{

	TRACE0();
	return 8192;	/* NetBSD/amiga PGSHIFT 13 */
}

/* ------------------------------------------------------------------------
 * files and block I/O: not provided (no file-backed rump devices)
 */

int
rumpuser_open(const char *path, int ruflags, int *fdp)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

int
rumpuser_close(int fd)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

int
rumpuser_getfileinfo(const char *path, uint64_t *sizep, int *ftp)
{

	TRACE0();
	return RUMPUSER_ENOENT;
}

void
rumpuser_bio(int fd, int op, void *data, size_t dlen, int64_t doff,
    rump_biodone_fn biodone, void *bioarg)
{

	TRACE0();
	biodone(bioarg, 0, RUMPUSER_EOPNOTSUPP);
}

int
rumpuser_iovread(int fd, struct rumpuser_iovec *ruiov, size_t iovlen,
    int64_t off, size_t *retp)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

int
rumpuser_iovwrite(int fd, const struct rumpuser_iovec *ruiov, size_t iovlen,
    int64_t off, size_t *retp)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

int
rumpuser_syncfd(int fd, int flags, uint64_t start, uint64_t len)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

/* ------------------------------------------------------------------------
 * clocks
 */

static void
clock_mono(int64_t *sec, long *nsec)
{
	struct EClockVal ev;
	unsigned long long ticks;

	ReadEClock(&ev);
	ticks = ((unsigned long long)ev.ev_hi << 32) | ev.ev_lo;
	*sec = (int64_t)(ticks / eclock_freq);
	*nsec = (long)((ticks % eclock_freq) * 1000000000ULL / eclock_freq);
}

int
rumpuser_clock_gettime(int enum_rumpclock, int64_t *sec, long *nsec)
{
	struct timeval tv;

	TRACE0();
	switch (enum_rumpclock) {
	case RUMPUSER_CLOCK_RELWALL:
		GetSysTime(&tv);
		*sec = (int64_t)tv.tv_secs + AMIGA_EPOCH_OFFSET;
		*nsec = (long)tv.tv_micro * 1000;
		return 0;
	case RUMPUSER_CLOCK_ABSMONO:
		clock_mono(sec, nsec);
		return 0;
	default:
		return RUMPUSER_EINVAL;
	}
}

/* sleep the calling host thread for a relative interval */
static void
host_sleep(int64_t sec, long nsec)
{
	struct timerequest *tr;

	if (sec < 0 || (sec == 0 && nsec <= 0))
		return;
	if ((tr = thread_timer(self())) == NULL)
		return;
	tr->tr_node.io_Command = TR_ADDREQUEST;
	tr->tr_time.tv_secs = (ULONG)sec;
	tr->tr_time.tv_micro = (ULONG)(nsec / 1000);
	if (sec == 0 && tr->tr_time.tv_micro == 0)
		tr->tr_time.tv_micro = 1;
	DoIO((struct IORequest *)tr);
}

int
rumpuser_clock_sleep(int enum_rumpclock, int64_t sec, long nsec)
{
	int64_t nowsec;
	long nownsec;

	TRACE0();
	switch (enum_rumpclock) {
	case RUMPUSER_CLOCK_RELWALL:
		break;
	case RUMPUSER_CLOCK_ABSMONO:
		clock_mono(&nowsec, &nownsec);
		sec -= nowsec;
		nsec -= nownsec;
		if (nsec < 0) {
			nsec += 1000000000;
			sec--;
		}
		break;
	default:
		return RUMPUSER_EINVAL;
	}
	KLOCK_WRAP(host_sleep(sec, nsec));
	return 0;
}

/* ------------------------------------------------------------------------
 * parameters: built-ins, otherwise DOS environment variables, so e.g.
 * "SetEnv RUMP_VERBOSE 1" works.
 */

/* GetVar() with DOS requesters suppressed (ENV: may not be assigned) */
static LONG
env_lookup(const char *name, void *buf, size_t blen)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR oldwin = me->pr_WindowPtr;
	LONG rv;

	me->pr_WindowPtr = (APTR)-1;
	rv = GetVar((CONST_STRPTR)name, buf, (LONG)blen, 0);
	me->pr_WindowPtr = oldwin;
	return rv;
}

int
rumpuser_getparam(const char *name, void *buf, size_t blen)
{
	const char *val = NULL;
	size_t l;

	TRACE0();
	if (host_strcmp(name, RUMPUSER_PARAM_NCPU) == 0)
		val = "1";
	else if (amiga_rump_debug && host_strcmp(name, "RUMP_VERBOSE") == 0)
		val = "1";
	else if (host_strcmp(name, RUMPUSER_PARAM_HOSTNAME) == 0)
		val = "amiga";

	if (val) {
		l = host_strlen(val);
		if (l + 1 > blen)
			return RUMPUSER_EINVAL;
		memcpy(buf, val, l + 1);
		return 0;
	}
	if (env_lookup(name, buf, blen) < 0)
		return RUMPUSER_ENOENT;
	return 0;
}

/* ------------------------------------------------------------------------
 * random: no hardware entropy source on a classic Amiga.  This mixes the
 * EClock and wall clock; good enough for TCP ISNs in a test setup only.
 * TODO: feed real entropy (input timing, SANA-II packet arrival jitter).
 */

static unsigned long long rnd_state;

int
rumpuser_getrandom(void *buf, size_t buflen, int flags, size_t *retp)
{
	unsigned char *p = buf;
	struct EClockVal ev;
	struct timeval tv;
	size_t i;

	TRACE0();
	ReadEClock(&ev);
	GetSysTime(&tv);
	rnd_state ^= ((unsigned long long)ev.ev_hi << 32 | ev.ev_lo) ^
	    ((unsigned long long)tv.tv_micro << 20) ^ tv.tv_secs;
	if (rnd_state == 0)
		rnd_state = 0x9e3779b97f4a7c15ULL;
	for (i = 0; i < buflen; i++) {
		rnd_state ^= rnd_state >> 12;
		rnd_state ^= rnd_state << 25;
		rnd_state ^= rnd_state >> 27;
		p[i] = (unsigned char)((rnd_state * 0x2545f4914f6cdd1dULL) >> 56);
	}
	*retp = buflen;
	return 0;
}

/* ------------------------------------------------------------------------
 * termination
 */

void
rumpuser_exit(int rv)
{

	if (rv == RUMPUSER_PANIC)
		amiga_rump_printf("rumpuser: rump kernel panic\n");
	else
		amiga_rump_printf("rumpuser: exit %d\n", rv);
	ObtainSemaphore(&logsem);
	log_flush_locked();
	ReleaseSemaphore(&logsem);
	amiga_rump_exitcode = rv;
	amiga_rump_exited = 1;
	if (amiga_rump_notifytask)
		Signal(amiga_rump_notifytask, SIGBREAKF_CTRL_C);
	else if (hosttask)
		Signal(hosttask, SIGBREAKF_CTRL_C);
	/* the host task decides what to do; this thread never returns */
	for (;;)
		Wait(SIGBREAKF_CTRL_F);
}

int
rumpuser_kill(int64_t pid, int sig)
{

	TRACE0();
	if (pid == RUMPUSER_PID_SELF)
		rumpuser_exit(RUMPUSER_PANIC);
	return 0;
}

void
rumpuser_seterrno(int error)
{

	self()->err = error;
}

/* the same for kernel-side AmiBSDNet code (keeps the rump name prefix) */
int
rumpuser_amiga_errno(void)
{

	return self()->err;
}

/* ------------------------------------------------------------------------
 * threads
 */

static void
thread_entry(void)
{
	struct Task *me = SysBase->ThisTask;
	struct amthread *t;

	/* wait until the creator has published our state */
	while ((t = me->tc_UserData) == NULL)
		Wait(SIGBREAKF_CTRL_F);

	/* a network stack must never put up DOS requesters */
	((struct Process *)me)->pr_WindowPtr = (APTR)-1;
	if (amiga_rump_debug)
		crash_install();

	if (thread_setup_self(t) == 0) {
		if (__builtin_setjmp(t->jmpbuf) == 0)
			t->func(t->arg);
	}
	thread_teardown_self(t);

	Forbid();		/* broken only when this process is gone */
	me->tc_UserData = NULL;
	t->done = 1;
	if (t->joinable) {
		if (t->joiner)
			Signal(t->joiner, t->joinmask);
	} else {
		FreeVec(t);
	}
}

int
rumpuser_thread_create(void *(*f)(void *), void *arg, const char *thrname,
    int joinable, int priority, int cpuidx, void **tptr)
{
	struct amthread *t;
	struct Process *proc;

	TRACE0();
	if ((t = thread_alloc()) == NULL)
		return RUMPUSER_ENOMEM;
	t->func = f;
	t->arg = arg;
	t->joinable = joinable;

	if (amiga_rump_debug)
		amiga_rump_printf("rumpuser: thread_create \"%s\" join=%d\n",
		    thrname ? thrname : "?", joinable);
	proc = CreateNewProcTags(
	    NP_Entry, (ULONG)thread_entry,
	    NP_Name, (ULONG)(thrname ? thrname : "rump thread"),
	    NP_StackSize, THREAD_STACK,
	    NP_Priority, 0,
	    TAG_DONE);
	if (proc == NULL) {
		FreeVec(t);
		return RUMPUSER_EAGAIN;
	}
	proc->pr_Task.tc_UserData = t;
	Signal(&proc->pr_Task, SIGBREAKF_CTRL_F);

	if (joinable)
		*tptr = t;
	return 0;
}

void
rumpuser_thread_exit(void)
{
	struct amthread *t = self();

	TRACE0();
	__builtin_longjmp(t->jmpbuf, 1);
}

int
rumpuser_thread_join(void *ptcookie)
{
	struct amthread *t = ptcookie, *me = self();

	TRACE0();
	KLOCK_WRAP(
		Forbid();
		while (!t->done) {
			t->joiner = SysBase->ThisTask;
			t->joinmask = me->cvmask;
			Wait(me->cvmask);
		}
		Permit();
	);
	FreeVec(t);
	return 0;
}

/*
 * The same for host code that is not on a rump CPU (component code
 * between rumpuser_component_unschedule() and _schedule(), like
 * pthread_join() in NetBSD's own backends): rumpuser_thread_join() is
 * the kernel's hypercall and would leave the caller holding a CPU.
 */
void
amiga_host_thread_join(void *ptcookie)
{
	struct amthread *t = ptcookie, *me = self();

	Forbid();
	while (!t->done) {
		t->joiner = SysBase->ThisTask;
		t->joinmask = me->cvmask;
		Wait(me->cvmask);
	}
	Permit();
	FreeVec(t);
}

/* whether a joinable thread has ended (also without running its
   function, if its setup failed) */
int
amiga_host_thread_done(void *ptcookie)
{

	return ((volatile struct amthread *)ptcookie)->done;
}

void
rumpuser_curlwpop(int enum_rumplwpop, struct lwp *l)
{
	struct amthread *t = self();

	TRACE0();
	switch (enum_rumplwpop) {
	case RUMPUSER_LWP_CREATE:
	case RUMPUSER_LWP_DESTROY:
		break;
	case RUMPUSER_LWP_SET:
		t->curlwp = l;
		break;
	case RUMPUSER_LWP_CLEAR:
		t->curlwp = NULL;
		break;
	}
}

struct lwp *
rumpuser_curlwp(void)
{
	struct amthread *t = self();

	return t ? t->curlwp : NULL;
}

/* ------------------------------------------------------------------------
 * mutexes
 */

struct rumpuser_mtx {
	struct SignalSemaphore sem;
	struct lwp *owner;
	int flags;
};

void
rumpuser_mutex_init(struct rumpuser_mtx **mtxp, int flags)
{
	struct rumpuser_mtx *mtx;

	TRACE0();
	if ((mtx = AllocVec(sizeof(*mtx), MEMF_FAST | MEMF_CLEAR)) == NULL)
		rumpuser_exit(RUMPUSER_PANIC);
	InitSemaphore(&mtx->sem);
	mtx->flags = flags;
	*mtxp = mtx;
}

int
rumpuser_mutex_spin_p(struct rumpuser_mtx *mtx)
{

	TRACE0();
	return (mtx->flags & RUMPUSER_MTX_SPIN) != 0;
}

static void
mtxenter(struct rumpuser_mtx *mtx)
{

	if (mtx->flags & RUMPUSER_MTX_KMUTEX)
		mtx->owner = rumpuser_curlwp();
}

static void
mtxexit(struct rumpuser_mtx *mtx)
{

	if (mtx->flags & RUMPUSER_MTX_KMUTEX)
		mtx->owner = NULL;
}

void
rumpuser_mutex_enter_nowrap(struct rumpuser_mtx *mtx)
{

	TRACE0();
	ObtainSemaphore(&mtx->sem);
	mtxenter(mtx);
}

void
rumpuser_mutex_enter(struct rumpuser_mtx *mtx)
{

	TRACE0();
	if (mtx->flags & RUMPUSER_MTX_SPIN) {
		rumpuser_mutex_enter_nowrap(mtx);
		return;
	}
	if (!AttemptSemaphore(&mtx->sem))
		KLOCK_WRAP(ObtainSemaphore(&mtx->sem));
	mtxenter(mtx);
}

int
rumpuser_mutex_tryenter(struct rumpuser_mtx *mtx)
{

	TRACE0();
	/* Exec semaphores nest; a pthread errorcheck mutex would not */
	if (mtx->sem.ss_Owner == SysBase->ThisTask)
		return RUMPUSER_EBUSY;
	if (!AttemptSemaphore(&mtx->sem))
		return RUMPUSER_EBUSY;
	mtxenter(mtx);
	return 0;
}

void
rumpuser_mutex_exit(struct rumpuser_mtx *mtx)
{

	TRACE0();
	mtxexit(mtx);
	ReleaseSemaphore(&mtx->sem);
}

void
rumpuser_mutex_destroy(struct rumpuser_mtx *mtx)
{

	TRACE0();
	FreeVec(mtx);
}

void
rumpuser_mutex_owner(struct rumpuser_mtx *mtx, struct lwp **lp)
{

	TRACE0();
	*lp = mtx->owner;
}

/* ------------------------------------------------------------------------
 * rwlocks (see the comment in rumpuser_pth.c about downgrade)
 */

struct rumpuser_rw {
	struct SignalSemaphore sem;
	volatile unsigned int readers;	/* (unsigned)-1 while write-held */
	struct lwp *writer;
	volatile int downgrade;
};

static int
rw_amwriter(struct rumpuser_rw *rw)
{

	return rw->writer == rumpuser_curlwp() && rw->readers == (unsigned)-1;
}

static int
rw_nreaders(struct rumpuser_rw *rw)
{
	unsigned n = rw->readers;

	return n != (unsigned)-1 ? n : 0;
}

static int
rw_setwriter(struct rumpuser_rw *rw, int retry)
{

	if (rw->downgrade) {
		ReleaseSemaphore(&rw->sem);
		if (retry)
			KLOCK_WRAP(host_sleep(0, 1000));
		return RUMPUSER_EBUSY;
	}
	rw->writer = rumpuser_curlwp();
	rw->readers = (unsigned)-1;
	return 0;
}

static void
rw_clearwriter(struct rumpuser_rw *rw)
{

	rw->readers = 0;
	rw->writer = NULL;
}

void
rumpuser_rw_init(struct rumpuser_rw **rwp)
{
	struct rumpuser_rw *rw;

	TRACE0();
	if ((rw = AllocVec(sizeof(*rw), MEMF_FAST | MEMF_CLEAR)) == NULL)
		rumpuser_exit(RUMPUSER_PANIC);
	InitSemaphore(&rw->sem);
	*rwp = rw;
}

void
rumpuser_rw_enter(int enum_rumprwlock, struct rumpuser_rw *rw)
{

	TRACE0();
	switch (enum_rumprwlock) {
	case RUMPUSER_RW_WRITER:
		do {
			if (!AttemptSemaphore(&rw->sem))
				KLOCK_WRAP(ObtainSemaphore(&rw->sem));
		} while (rw_setwriter(rw, 1) != 0);
		break;
	case RUMPUSER_RW_READER:
		if (!AttemptSemaphoreShared(&rw->sem))
			KLOCK_WRAP(ObtainSemaphoreShared(&rw->sem));
		__atomic_add_fetch(&rw->readers, 1, __ATOMIC_SEQ_CST);
		break;
	}
}

int
rumpuser_rw_tryenter(int enum_rumprwlock, struct rumpuser_rw *rw)
{

	TRACE0();
	switch (enum_rumprwlock) {
	case RUMPUSER_RW_WRITER:
		if (!AttemptSemaphore(&rw->sem))
			return RUMPUSER_EBUSY;
		return rw_setwriter(rw, 0);
	case RUMPUSER_RW_READER:
		if (!AttemptSemaphoreShared(&rw->sem))
			return RUMPUSER_EBUSY;
		__atomic_add_fetch(&rw->readers, 1, __ATOMIC_SEQ_CST);
		return 0;
	default:
		return RUMPUSER_EINVAL;
	}
}

int
rumpuser_rw_tryupgrade(struct rumpuser_rw *rw)
{

	TRACE0();
	/* callers must back off and retry anyway; always failing is correct */
	return RUMPUSER_EBUSY;
}

void
rumpuser_rw_exit(struct rumpuser_rw *rw)
{

	TRACE0();
	if (rw_nreaders(rw))
		__atomic_sub_fetch(&rw->readers, 1, __ATOMIC_SEQ_CST);
	else
		rw_clearwriter(rw);
	ReleaseSemaphore(&rw->sem);
}

void
rumpuser_rw_downgrade(struct rumpuser_rw *rw)
{

	TRACE0();
	rw->downgrade = 1;
	rumpuser_rw_exit(rw);
	KLOCK_WRAP(ObtainSemaphoreShared(&rw->sem));
	rw->downgrade = 0;
	__atomic_add_fetch(&rw->readers, 1, __ATOMIC_SEQ_CST);
}

void
rumpuser_rw_destroy(struct rumpuser_rw *rw)
{

	TRACE0();
	FreeVec(rw);
}

void
rumpuser_rw_held(int enum_rumprwlock, struct rumpuser_rw *rw, int *rv)
{

	TRACE0();
	switch (enum_rumprwlock) {
	case RUMPUSER_RW_WRITER:
		*rv = rw_amwriter(rw);
		break;
	case RUMPUSER_RW_READER:
		*rv = rw_nreaders(rw);
		break;
	}
}

/* ------------------------------------------------------------------------
 * condition variables
 */

struct cvwaiter {
	struct MinNode node;
	struct Task *task;
	ULONG mask;
	volatile int woken;
};

struct rumpuser_cv {
	struct MinList waiters;
	int nwaiters;
};

void
rumpuser_cv_init(struct rumpuser_cv **cvp)
{
	struct rumpuser_cv *cv;

	TRACE0();
	if ((cv = AllocVec(sizeof(*cv), MEMF_FAST | MEMF_CLEAR)) == NULL)
		rumpuser_exit(RUMPUSER_PANIC);
	/* NewList() lives in amiga.lib, which is not linked */
	cv->waiters.mlh_Head = (struct MinNode *)&cv->waiters.mlh_Tail;
	cv->waiters.mlh_Tail = NULL;
	cv->waiters.mlh_TailPred = (struct MinNode *)&cv->waiters.mlh_Head;
	*cvp = cv;
}

void
rumpuser_cv_destroy(struct rumpuser_cv *cv)
{

	TRACE0();
	FreeVec(cv);
}

static void
cv_unschedule(struct rumpuser_mtx *mtx, int *nlocks)
{

	rumpkern_unsched(nlocks, mtx);
	mtxexit(mtx);
}

static void
cv_reschedule(struct rumpuser_mtx *mtx, int nlocks)
{

	/* same resource ordering rules as rumpuser_pth.c */
	if ((mtx->flags & (RUMPUSER_MTX_SPIN | RUMPUSER_MTX_KMUTEX)) ==
	    (RUMPUSER_MTX_SPIN | RUMPUSER_MTX_KMUTEX)) {
		ReleaseSemaphore(&mtx->sem);
		rumpkern_sched(nlocks, mtx);
		rumpuser_mutex_enter_nowrap(mtx);
	} else {
		mtxenter(mtx);
		rumpkern_sched(nlocks, mtx);
	}
}

/*
 * Release mtx, sleep until signalled or the timeout (relative, NULL for
 * none) expires, re-obtain mtx.  Returns 0 if signalled, ETIMEDOUT if not.
 */
static int
cv_sleep(struct rumpuser_cv *cv, struct rumpuser_mtx *mtx,
    const struct timeval *tmo)
{
	struct amthread *t = self();
	struct timerequest *tr = NULL;
	struct cvwaiter w;
	ULONG tmask = 0;
	int rv = 0;

	w.task = SysBase->ThisTask;
	w.mask = t->cvmask;
	w.woken = 0;

	if (tmo && (tr = thread_timer(t)) != NULL) {
		tmask = 1UL << t->tport->mp_SigBit;
		tr->tr_node.io_Command = TR_ADDREQUEST;
		tr->tr_time = *tmo;
	}

	Forbid();
	SetSignal(0, w.mask);
	AddTail((struct List *)&cv->waiters, (struct Node *)&w.node);
	cv->nwaiters++;
	ReleaseSemaphore(&mtx->sem);
	if (tr)
		SendIO((struct IORequest *)tr);
	while (!w.woken) {
		ULONG got = Wait(w.mask | tmask);

		if (!w.woken && tr && (got & tmask) &&
		    CheckIO((struct IORequest *)tr)) {
			rv = RUMPUSER_ETIMEDOUT;
			break;
		}
	}
	if (!w.woken)
		Remove((struct Node *)&w.node);	/* signaller removes on wake */
	cv->nwaiters--;
	Permit();

	if (tr) {
		if (!CheckIO((struct IORequest *)tr))
			AbortIO((struct IORequest *)tr);
		WaitIO((struct IORequest *)tr);
	}
	if (w.woken)
		rv = 0;
	ObtainSemaphore(&mtx->sem);
	return rv;
}

void
rumpuser_cv_wait(struct rumpuser_cv *cv, struct rumpuser_mtx *mtx)
{
	int nlocks;

	TRACE0();
	cv_unschedule(mtx, &nlocks);
	cv_sleep(cv, mtx, NULL);
	cv_reschedule(mtx, nlocks);
}

void
rumpuser_cv_wait_nowrap(struct rumpuser_cv *cv, struct rumpuser_mtx *mtx)
{

	TRACE0();
	mtxexit(mtx);
	cv_sleep(cv, mtx, NULL);
	mtxenter(mtx);
}

int
rumpuser_cv_timedwait(struct rumpuser_cv *cv, struct rumpuser_mtx *mtx,
    int64_t sec, int64_t nsec)
{
	struct timeval tv;
	int nlocks, rv;

	TRACE0();
	if (sec < 0)
		sec = 0, nsec = 0;
	tv.tv_secs = (ULONG)sec;
	tv.tv_micro = (ULONG)(nsec / 1000);
	if (tv.tv_secs == 0 && tv.tv_micro == 0)
		tv.tv_micro = 1;

	cv_unschedule(mtx, &nlocks);
	rv = cv_sleep(cv, mtx, &tv);
	cv_reschedule(mtx, nlocks);
	return rv;
}

static void
cv_wake(struct rumpuser_cv *cv, int all)
{
	struct cvwaiter *w;

	Forbid();
	while ((w = (struct cvwaiter *)RemHead((struct List *)&cv->waiters))) {
		w->woken = 1;
		Signal(w->task, w->mask);
		if (!all)
			break;
	}
	Permit();
}

void
rumpuser_cv_signal(struct rumpuser_cv *cv)
{

	TRACE0();
	cv_wake(cv, 0);
}

void
rumpuser_cv_broadcast(struct rumpuser_cv *cv)
{

	TRACE0();
	cv_wake(cv, 1);
}

void
rumpuser_cv_has_waiters(struct rumpuser_cv *cv, int *nwaiters)
{

	TRACE0();
	*nwaiters = cv->nwaiters;
}

/* ------------------------------------------------------------------------
 * rumpuser_component(3): lets host-side driver code (e.g. the SANA-II
 * backend) enter and leave the rump kernel.  Same as librumpuser.
 */

void *
rumpuser_component_unschedule(void)
{
	int nlocks;

	rumpkern_unsched(&nlocks, NULL);
	return (void *)(intptr_t)nlocks;
}

void
rumpuser_component_schedule(void *cookie)
{

	rumpkern_sched((int)(intptr_t)cookie, NULL);
}

void
rumpuser_component_kthread(void)
{

	hyp.hyp_schedule();
	hyp.hyp_lwproc_newlwp(0);
	hyp.hyp_unschedule();
}

void
rumpuser_component_kthread_release(void)
{

	hyp.hyp_schedule();
	hyp.hyp_lwproc_release();
	hyp.hyp_unschedule();
}

struct lwp *
rumpuser_component_curlwp(void)
{
	struct lwp *l;

	hyp.hyp_schedule();
	l = hyp.hyp_lwproc_curlwp();
	hyp.hyp_unschedule();
	return l;
}

void
rumpuser_component_switchlwp(struct lwp *l)
{

	hyp.hyp_schedule();
	hyp.hyp_lwproc_switch(l);
	hyp.hyp_unschedule();
}

int
rumpuser_component_errtrans(int hosterr)
{

	return hosterr;		/* host code already uses NetBSD errno values */
}

void
amiga_host_sleep_ms(unsigned long ms)
{

	host_sleep((int64_t)(ms / 1000), (long)(ms % 1000) * 1000000L);
}

/* ------------------------------------------------------------------------
 * dynamic loading, daemonising, syscall proxy: not applicable.
 * With RUMP_USE_CTOR, components register themselves from constructors.
 */

void
rumpuser_dl_bootstrap(rump_modinit_fn domodinit, rump_symload_fn symload,
    rump_compload_fn compload, rump_evcntattach_fn doevcntattach)
{
	TRACE0();
}

int
rumpuser_daemonize_begin(void)
{

	TRACE0();
	return 0;
}

int
rumpuser_daemonize_done(int error)
{

	TRACE0();
	return 0;
}

int
rumpuser_sp_init(const char *url, const char *ostype, const char *osrelease,
    const char *machine)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

int
rumpuser_sp_copyin(void *arg, const void *raddr, void *laddr, size_t len)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

int
rumpuser_sp_copyinstr(void *arg, const void *raddr, void *laddr, size_t *lenp)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

int
rumpuser_sp_copyout(void *arg, const void *laddr, void *raddr, size_t dlen)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

int
rumpuser_sp_copyoutstr(void *arg, const void *laddr, void *raddr,
    size_t *dlenp)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

int
rumpuser_sp_anonmmap(void *arg, size_t howmuch, void **addr)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

int
rumpuser_sp_raise(void *arg, int signo)
{

	TRACE0();
	return RUMPUSER_ENOSYS;
}

void
rumpuser_sp_fini(void *arg)
{
	TRACE0();
}

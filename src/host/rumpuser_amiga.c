/*
 * rumpuser(3) hypercalls for AmigaOS (Exec/DOS), RUMPUSER_VERSION 17.
 *
 * Model: lib/librumpuser/rumpuser_pth.c, with pthreads replaced by Exec.
 *
 *  - rump threads are DOS processes; per-thread state hangs off
 *    tc_UserData (struct amthread), which the new process sets itself
 *    from a startup message (see thread_entry()).  RUMP_CURLWP_HYPERCALL
 *    means curlwp is fetched through rumpuser_curlwp(), so no TLS is
 *    needed.
 *  - mutexes and rwlocks are SignalSemaphores plus the owner/reader
 *    bookkeeping rump expects.
 *  - condition variables are waiter lists manipulated under Forbid(),
 *    woken with a per-thread signal bit.  Waiting inside Forbid() is the
 *    standard Exec idiom: Wait() breaks the Forbid while asleep, so the
 *    "release mutex, sleep" step cannot lose a wakeup.
 *  - timeouts and sleeps use a per-thread timer.device request, opened
 *    when the thread is set up (so a timed wait can never be without it).
 *
 * Every blocking host operation is bracketed by KLOCK_WRAP(), which gives
 * the rump virtual CPU back while the host thread sleeps.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/ports.h>
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
#include "entropy.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
struct Device *TimerBase;

/* seconds from the AmigaOS epoch (1978-01-01) to the Unix epoch */
#define	AMIGA_EPOCH_OFFSET	252460800UL

#define	THREAD_STACK		(32 * 1024)	/* NetBSD/m68k uses 16 KB */

/*
 * Memory shared between tasks must be MEMF_PUBLIC ("ALL MEMORY THAT IS
 * REFERENCED VIA INTERRUPTS AND/OR BY OTHER TASKS MUST BE EITHER PUBLIC
 * OR LOCKED INTO MEMORY", exec.doc AllocMem).  No MEMF_FAST: with no
 * memory type requested "the fast memory pool is searched first", and
 * MEMF_FAST would make the allocation fail where only Chip RAM is left
 * (same autodoc).
 */
#define	MEMF_SHARED		MEMF_PUBLIC

struct Task *amiga_rump_notifytask;
void (*amiga_rump_exitfn)(void);
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
static int hostinited;
static BPTR logfh;
static struct SignalSemaphore logsem;
static volatile int loginited;
static ULONG eclock_freq;

static void fatal(const char *, int) __attribute__((__noreturn__));

/* ------------------------------------------------------------------------
 * per-thread state
 */

struct amthread {
	struct lwp *curlwp;
	int err;

	BYTE cvsig;			/* -1 until allocated */
	ULONG cvmask;

	struct MsgPort *tport;		/* timer, opened at set-up */
	struct timerequest *treq;

	/* creation / join */
	void *(*func)(void *);
	void *arg;
	int joinable;			/* Forbid() protects */
	volatile int done;
	struct Task *joiner;
	ULONG joinmask;
	int crashtrap;			/* crash_install() was called */

	APTR saved_userdata;		/* for adopted (non-rump) tasks */
	void *jmpbuf[5];		/* __builtin_setjmp buffer */
};

/* sent by the creator to the new process (see thread_entry()) */
struct startmsg {
	struct Message msg;
	struct amthread *t;
	int err;			/* set by the new process */
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

	t = AllocVec(sizeof(*t), MEMF_SHARED | MEMF_CLEAR);
	if (t)
		t->cvsig = -1;
	return t;
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

/* called on the thread itself: signal bits and the timer's reply port
   belong to the calling task */
static int
thread_setup_self(struct amthread *t)
{

	t->cvsig = AllocSignal(-1);
	if (t->cvsig == -1)
		return RUMPUSER_EAGAIN;
	t->cvmask = 1UL << t->cvsig;
	if ((t->tport = CreateMsgPort()) == NULL)
		goto fail;
	t->treq = (struct timerequest *)CreateIORequest(t->tport,
	    sizeof(struct timerequest));
	if (t->treq == NULL)
		goto fail;
	if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
	    (struct IORequest *)t->treq, 0) != 0) {
		DeleteIORequest((struct IORequest *)t->treq);
		t->treq = NULL;
		goto fail;
	}
	return 0;
fail:
	thread_teardown_self(t);
	return RUMPUSER_EAGAIN;
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
static void (*logtee)(const char *, long);	/* also shown here */

/*
 * The semaphore is set up on first use, whoever logs first (also before
 * amiga_rump_loginit()); Forbid() makes that one-time set-up atomic.
 */
static void
log_lock(void)
{

	if (!loginited) {
		Forbid();
		if (!loginited) {
			InitSemaphore(&logsem);
			loginited = 1;
		}
		Permit();
	}
	ObtainSemaphore(&logsem);
}

static void
log_unlock(void)
{

	ReleaseSemaphore(&logsem);
}

static void
log_flush_locked(void)
{

	if (loglen && logfh)
		Write(logfh, logbuf, loglen);
	if (loglen && logtee)
		logtee(logbuf, (long)loglen);
	loglen = 0;
}

/* everything logged from now on is also given to fn (NULL: no more);
   never while fn runs */
void
amiga_rump_logtee(void (*fn)(const char *, long))
{

	log_lock();
	logtee = fn;
	log_unlock();
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

	log_lock();
	log_putc_locked(c);
	log_unlock();
}

/*
 * printf-style formatting, shared by the C (va_list) and the Amiga
 * (packed LONG array) entry points: %d %i %u %o %x %X %p %c %s %%, the
 * flags - 0 + space #, a width and a precision (both also '*'), and the
 * length modifiers hh h l ll z j t.  A conversion the formatter does not
 * know is printed as it is and, like any conversion, takes one argument,
 * so that the later ones stay in place.
 */

enum { LEN_HH, LEN_H, LEN_INT, LEN_L, LEN_LL, LEN_Z };

struct fmtspec {
	int	left, zero, plus, space, alt;
	int	width, prec;		/* prec -1: none */
	int	len;
	char	conv;
};

struct fmtargs {
	va_list	*ap;			/* C arguments, or */
	const LONG *longs;		/* one LONG per argument (NULL ok) */
};

static void
pad_out(int n, char c)
{

	while (n-- > 0)
		log_putc_locked(c);
}

static long long
arg_signed(struct fmtargs *a, int len)
{
	long long v;

	if (a->ap == NULL)
		v = a->longs ? *a->longs++ : 0;
	else if (len == LEN_LL)
		v = va_arg(*a->ap, long long);
	else if (len == LEN_L)
		v = va_arg(*a->ap, long);
	else if (len == LEN_Z)
		v = (long long)va_arg(*a->ap, size_t);
	else
		v = va_arg(*a->ap, int);
	if (len == LEN_H)
		v = (short)v;
	else if (len == LEN_HH)
		v = (signed char)v;
	return v;
}

static unsigned long long
arg_unsigned(struct fmtargs *a, int len)
{
	unsigned long long v;

	if (a->ap == NULL)
		v = a->longs ? (ULONG)*a->longs++ : 0;
	else if (len == LEN_LL)
		v = va_arg(*a->ap, unsigned long long);
	else if (len == LEN_L)
		v = va_arg(*a->ap, unsigned long);
	else if (len == LEN_Z)
		v = va_arg(*a->ap, size_t);
	else
		v = va_arg(*a->ap, unsigned int);
	if (len == LEN_H)
		v = (unsigned short)v;
	else if (len == LEN_HH)
		v = (unsigned char)v;
	return v;
}

static int
arg_int(struct fmtargs *a)
{

	if (a->ap == NULL)
		return a->longs ? (int)*a->longs++ : 0;
	return va_arg(*a->ap, int);
}

static const void *
arg_ptr(struct fmtargs *a)
{

	if (a->ap == NULL)
		return a->longs ? (const void *)*a->longs++ : NULL;
	return va_arg(*a->ap, const void *);
}

static void
emit_num(const struct fmtspec *f, unsigned long long u, int neg)
{
	char digits[24], pfx[3];
	const char *set = f->conv == 'X' ? "0123456789ABCDEF" :
	    "0123456789abcdef";
	int base, nd = 0, np = 0, zeros = 0, total, i;
	unsigned long long v = u;

	base = f->conv == 'o' ? 8 :
	    (f->conv == 'x' || f->conv == 'X' || f->conv == 'p') ? 16 : 10;
	/* C: precision 0 and value 0 give no digits */
	if (!(v == 0 && f->prec == 0))
		do {
			digits[nd++] = set[v % base];
			v /= base;
		} while (v);
	if (neg)
		pfx[np++] = '-';
	else if ((f->conv == 'd' || f->conv == 'i') && f->plus)
		pfx[np++] = '+';
	else if ((f->conv == 'd' || f->conv == 'i') && f->space)
		pfx[np++] = ' ';
	else if (f->conv == 'p' || ((f->conv == 'x' || f->conv == 'X') &&
	    f->alt && u != 0)) {
		pfx[np++] = '0';
		pfx[np++] = f->conv == 'X' ? 'X' : 'x';
	}
	if (f->prec > nd)
		zeros = f->prec - nd;
	if (f->conv == 'o' && f->alt && zeros == 0 &&
	    (nd == 0 || digits[nd - 1] != '0'))
		zeros = 1;
	total = np + zeros + nd;
	if (!f->left && f->zero && f->prec < 0 && f->width > total) {
		zeros += f->width - total;
		total = f->width;
	}
	if (!f->left)
		pad_out(f->width - total, ' ');
	for (i = 0; i < np; i++)
		log_putc_locked(pfx[i]);
	pad_out(zeros, '0');
	while (nd)
		log_putc_locked(digits[--nd]);
	if (f->left)
		pad_out(f->width - total, ' ');
}

static void
emit_str(const struct fmtspec *f, const char *s, int n)
{
	int i;

	if (!f->left)
		pad_out(f->width - n, ' ');
	for (i = 0; i < n; i++)
		log_putc_locked(s[i]);
	if (f->left)
		pad_out(f->width - n, ' ');
}

static void
format_locked(const char *fmt, struct fmtargs *a)
{
	struct fmtspec f;
	const char *str;
	long long s;
	char c;
	int n;

	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			log_putc_locked(*fmt);
			continue;
		}
		fmt++;
		f.left = f.zero = f.plus = f.space = f.alt = 0;
		f.width = 0;
		f.prec = -1;
		for (;; fmt++) {
			if (*fmt == '-')
				f.left = 1;
			else if (*fmt == '0')
				f.zero = 1;
			else if (*fmt == '+')
				f.plus = 1;
			else if (*fmt == ' ')
				f.space = 1;
			else if (*fmt == '#')
				f.alt = 1;
			else
				break;
		}
		if (*fmt == '*') {
			f.width = arg_int(a);
			if (f.width < 0)
				f.left = 1, f.width = -f.width;
			fmt++;
		} else
			while (*fmt >= '0' && *fmt <= '9')
				f.width = f.width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			fmt++;
			f.prec = 0;
			if (*fmt == '*') {
				f.prec = arg_int(a);
				if (f.prec < 0)
					f.prec = -1;	/* as if omitted */
				fmt++;
			} else
				while (*fmt >= '0' && *fmt <= '9')
					f.prec = f.prec * 10 + (*fmt++ - '0');
		}
		f.len = LEN_INT;
		if (fmt[0] == 'h' && fmt[1] == 'h')
			f.len = LEN_HH, fmt += 2;
		else if (fmt[0] == 'h')
			f.len = LEN_H, fmt++;
		else if (fmt[0] == 'l' && fmt[1] == 'l')
			f.len = LEN_LL, fmt += 2;
		else if (fmt[0] == 'l')
			f.len = LEN_L, fmt++;
		else if (fmt[0] == 'j')
			f.len = LEN_LL, fmt++;	/* intmax_t: long long */
		else if (fmt[0] == 'z' || fmt[0] == 't')
			f.len = LEN_Z, fmt++;	/* 32 bits, as size_t */
		/* (width bound: a garbage width must not flood the log) */
		if (f.width > 200)
			f.width = 200;
		f.conv = *fmt;
		switch (f.conv) {
		case 'd':
		case 'i':
			s = arg_signed(a, f.len);
			emit_num(&f, s < 0 ? -(unsigned long long)s :
			    (unsigned long long)s, s < 0);
			break;
		case 'u':
		case 'o':
		case 'x':
		case 'X':
			emit_num(&f, arg_unsigned(a, f.len), 0);
			break;
		case 'p':
			f.prec = 8;
			emit_num(&f, (unsigned long)arg_ptr(a), 0);
			break;
		case 'c':
			c = (char)arg_int(a);
			emit_str(&f, &c, 1);
			break;
		case 's':
			str = arg_ptr(a);
			if (str == NULL)
				str = "(null)";
			for (n = 0; str[n] && (f.prec < 0 || n < f.prec); n++)
				;
			emit_str(&f, str, n);
			break;
		case '%':
			log_putc_locked('%');
			break;
		case '\0':
			fmt--;
			break;
		default:
			log_putc_locked('%');
			log_putc_locked(f.conv);
			(void)arg_unsigned(a, f.len);
			break;
		}
	}
}

void
amiga_rump_vprintf(const char *fmt, va_list ap)
{
	struct fmtargs a;
	va_list cp;

	va_copy(cp, ap);
	a.ap = &cp;
	a.longs = NULL;
	log_lock();
	format_locked(fmt, &a);
	log_unlock();
	va_end(cp);
}

/*
 * Format with Amiga-style arguments: a packed array of LONGs (vsyslog()
 * convention), one LONG for every conversion, '*' width and precision.
 */
void
amiga_rump_vprintf_longs(const char *fmt, const LONG *args)
{
	struct fmtargs a;

	a.ap = NULL;
	a.longs = args;
	log_lock();
	format_locked(fmt, &a);
	log_unlock();
}

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

/* librumpuser's NOFAIL_ERRNO() (rumpuser_int.h:87-95): report, then end
   the rump kernel as a panic */
static void
fatal(const char *what, int error)
{

	amiga_rump_printf("panic: rumpuser fatal failure %d (%s)\n", error,
	    what);
	rumpuser_exit(RUMPUSER_PANIC);
}

/* ------------------------------------------------------------------------
 * init
 */

/*
 * Log to the DOS file handle 'log' from now on (0: nothing more to a
 * file); what is buffered goes to the old one first.  Returns the old
 * handle, which nothing writes to any more once this returns (every
 * Write() happens under logsem), so the caller may Close() it.
 */
long
amiga_rump_logswitch(long log)
{
	BPTR old;

	log_lock();
	log_flush_locked();
	old = logfh;
	logfh = (BPTR)log;
	log_unlock();
	return (long)old;
}

void
amiga_rump_loginit(long log)
{

	(void)amiga_rump_logswitch(log);
}

int
amiga_rump_hostinit(long log)
{
	struct amthread *t;
	struct EClockVal ev;
	int rv;

	if (log)
		amiga_rump_loginit(log);
	if (hostinited)
		return SysBase->ThisTask == hosttask ? 0 : RUMPUSER_EBUSY;
	if ((t = thread_alloc()) == NULL)
		return RUMPUSER_ENOMEM;
	if ((rv = thread_setup_self(t)) != 0) {
		FreeVec(t);
		return rv;
	}
	hosttask = SysBase->ThisTask;
	t->saved_userdata = hosttask->tc_UserData;
	hosttask->tc_UserData = t;
	TimerBase = t->treq->tr_node.io_Device;
	eclock_freq = ReadEClock(&ev);
	amiga_entropy_init();
	hostinited = 1;
	return 0;
}

void
amiga_rump_hostfini(void)
{
	struct amthread *t;

	if (!hostinited || SysBase->ThisTask != hosttask)
		return;
	t = self();
	/* (TimerBase stays: rump threads may still read the clock, and
	   timer.device stays open for as long as they have it open) */
	hosttask->tc_UserData = t->saved_userdata;
	thread_teardown_self(t);
	FreeVec(t);
	hostinited = 0;
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
	if (!hostinited) {
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
 * Blocks come straight from AllocMem() (MEMF_SHARED, see above) and go
 * back with FreeMem() using the length rump passes to rumpuser_free()
 * (always the allocation length).  AllocMem() blocks are aligned to
 * MEM_BLOCKSIZE, 8 bytes (exec/memory.h).  Larger alignments (the kernel
 * asks for whole 8 KB pages) allocate size + alignment, then FreeMem()
 * the unused head and tail, both multiples of MEM_BLOCKSIZE.
 */

#define	MEM_ROUND(n)	(((n) + MEM_BLOCKMASK) & ~(size_t)MEM_BLOCKMASK)

static int
host_alloc(size_t len, int alignment, ULONG flags, void **memp)
{
	size_t size = MEM_ROUND(len ? len : 1), total, head, tail;
	UBYTE *raw, *a;

	if (alignment <= MEM_BLOCKSIZE) {
		if ((raw = AllocMem(size, MEMF_SHARED | flags)) == NULL)
			return RUMPUSER_ENOMEM;
		*memp = raw;
		return 0;
	}
	if ((alignment & (alignment - 1)) != 0)
		return RUMPUSER_EINVAL;
	total = size + alignment;
	if ((raw = AllocMem(total, MEMF_SHARED | flags)) == NULL)
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

int
rumpuser_malloc(size_t len, int alignment, void **memp)
{

	TRACE0();
	return host_alloc(len, alignment, 0, memp);
}

void
rumpuser_free(void *mem, size_t len)
{

	TRACE0();
	if (mem)
		FreeMem(mem, MEM_ROUND(len ? len : 1));
}

/* anonymous memory, like mmap(MAP_ANON) in librumpuser: zero-filled */
int
rumpuser_anonmmap(void *prefaddr, size_t size, int alignbit, int exec,
    void **memp)
{

	TRACE0();
	return host_alloc(size, 1 << alignbit, MEMF_CLEAR, memp);
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

/*
 * A relative interval as a timeval: "The microseconds must always be
 * normalized e.g. the longword must be between 0 and one million"
 * (timer.doc, TIMEVAL).  Returns 0 for an interval that is over already.
 */
static int
make_timeval(int64_t sec, int64_t nsec, struct timeval *tv)
{

	if (nsec >= 1000000000 || nsec <= -1000000000) {
		sec += nsec / 1000000000;
		nsec %= 1000000000;
	}
	if (nsec < 0) {
		nsec += 1000000000;
		sec--;
	}
	if (sec < 0 || (sec == 0 && nsec == 0))
		return 0;
	if (sec > 0xffffffffLL) {
		tv->tv_secs = 0xffffffffUL;
		tv->tv_micro = 999999;
		return 1;
	}
	tv->tv_secs = (ULONG)sec;
	tv->tv_micro = (ULONG)(nsec / 1000);
	if (tv->tv_secs == 0 && tv->tv_micro == 0)
		tv->tv_micro = 1;	/* under 1 us: still a wait */
	return 1;
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

/*
 * Sleep the calling host task for a relative interval.  Rump threads and
 * the host task have their timer from set-up; any other task gets one for
 * the call, and if even that cannot be had, a DOS process sleeps with
 * Delay() (dos.doc: "ticks (50 per second)"), rounded up.
 */
static void
host_sleep(int64_t sec, long nsec)
{
	struct amthread *t = self();
	struct timerequest *tr;
	struct MsgPort *port = NULL;
	struct timeval tv;
	struct Task *me = SysBase->ThisTask;

	if (!make_timeval(sec, nsec, &tv))
		return;
	if (t != NULL && t->treq != NULL)
		tr = t->treq;
	else {
		tr = NULL;
		if ((port = CreateMsgPort()) != NULL &&
		    (tr = (struct timerequest *)CreateIORequest(port,
		    sizeof(*tr))) != NULL &&
		    OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
		    (struct IORequest *)tr, 0) != 0) {
			DeleteIORequest((struct IORequest *)tr);
			tr = NULL;
		}
		if (tr == NULL) {
			if (port)
				DeleteMsgPort(port);
			if (me->tc_Node.ln_Type != NT_PROCESS)
				fatal("no timer for a sleep", RUMPUSER_EAGAIN);
			Delay(tv.tv_secs * 50 + (tv.tv_micro + 19999) / 20000);
			return;
		}
	}
	tr->tr_node.io_Command = TR_ADDREQUEST;
	tr->tr_time = tv;
	DoIO((struct IORequest *)tr);
	if (port) {
		CloseDevice((struct IORequest *)tr);
		DeleteIORequest((struct IORequest *)tr);
		DeleteMsgPort(port);
	}
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
		nsec -= nownsec;	/* (make_timeval() normalises) */
		break;
	default:
		return RUMPUSER_EINVAL;
	}
	KLOCK_WRAP(host_sleep(sec, nsec));
	return 0;
}

/* ------------------------------------------------------------------------
 * parameters, as librumpuser's rumpuser_getparam() (rumpuser.c:186-221):
 * the mandatory "_" names are built in, every other "_" name is EINVAL,
 * the rest are DOS variables (GetVar), so e.g. "SetEnv RUMP_VERBOSE 1"
 * works.  librumpuser returns getenv_r()'s errno; librumpuser's own
 * getenv_r() (rumpuser_port.h:159-174) gives ERANGE when the value does
 * not fit (strlen >= buflen) and ENOENT when there is no such variable.
 */

#define	RUMPUSER_ERANGE		34

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
	char *tmp;
	LONG got;
	size_t l;

	TRACE0();
	if (host_strcmp(name, RUMPUSER_PARAM_NCPU) == 0)
		val = "1";
	else if (host_strcmp(name, RUMPUSER_PARAM_HOSTNAME) == 0)
		val = "amiga";
	else if (*name == '_')
		return RUMPUSER_EINVAL;
	else if (amiga_rump_debug && host_strcmp(name, "RUMP_VERBOSE") == 0)
		val = "1";

	if (val) {
		l = host_strlen(val);
		if (l + 1 > blen)
			return RUMPUSER_ERANGE;
		memcpy(buf, val, l + 1);
		return 0;
	}
	if (blen == 0)
		return RUMPUSER_ERANGE;
	/*
	 * GetVar() truncates a value that does not fit and returns "the
	 * number of characters put in the buffer" (dos.doc GetVar).  One
	 * byte more of room tells a value that just fits from one that was
	 * cut.
	 */
	if ((tmp = AllocVec(blen + 1, MEMF_ANY)) == NULL)
		return RUMPUSER_ENOMEM;
	got = env_lookup(name, tmp, blen + 1);
	if (got < 0) {
		FreeVec(tmp);
		return RUMPUSER_ENOENT;
	}
	if ((size_t)got + 1 > blen) {
		FreeVec(tmp);
		return RUMPUSER_ERANGE;
	}
	memcpy(buf, tmp, (size_t)got + 1);
	FreeVec(tmp);
	return 0;
}

/* ------------------------------------------------------------------------
 * random: see entropy.c.  The kernel's caller, hyperentropy.c:41-60,
 * loops while the call returns 0 and stops at the first error, so HARD
 * with NOWAIT returns RUMPUSER_EAGAIN when no credited entropy is there,
 * never 0 with nothing written (that would make it loop for ever).
 */

int
rumpuser_getrandom(void *buf, size_t buflen, int flags, size_t *retp)
{
	int hard = (flags & RUMPUSER_RANDOM_HARD) != 0;
	size_t n;

	TRACE0();
	*retp = 0;
	if (buflen == 0)
		return 0;
	/* when the kernel asks is timing too (never credited) */
	amiga_entropy_event(ENT_SRC_HOST, 0, NULL, 0);
	while ((n = amiga_entropy_extract(buf, buflen, hard)) == 0) {
		if (flags & RUMPUSER_RANDOM_NOWAIT)
			return RUMPUSER_EAGAIN;
		KLOCK_WRAP(amiga_entropy_wait(self()->cvmask));
	}
	*retp = n;
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
	log_lock();
	log_flush_locked();
	log_unlock();
	amiga_rump_exitcode = rv;
	amiga_rump_exited = 1;
	/* the task to tell is this one: a signal would never be seen */
	if (amiga_rump_exitfn && SysBase->ThisTask == (amiga_rump_notifytask ?
	    amiga_rump_notifytask : hosttask))
		amiga_rump_exitfn();
	if (amiga_rump_notifytask)
		Signal(amiga_rump_notifytask, SIGBREAKF_CTRL_C);
	else if (hosttask)
		Signal(hosttask, SIGBREAKF_CTRL_C);
	/* the host task decides what to do; this thread never returns */
	for (;;)
		Wait(SIGBREAKF_CTRL_F);
}

/*
 * rumpuser.3: "advises the hypercall implementation to raise a signal
 * for the process containing the rump kernel ... In case there is no
 * mapping between sig and native signals (if any), the behavior is
 * implementation-defined", and "A rump kernel will ignore the return
 * value".  AmigaOS has no POSIX signals for a process, so there is
 * nothing to raise: the kernel's default model (RUMP_SIGMODEL_RAISE,
 * signals.c:89-97) calls this for every signal it posts to a process.
 */
int
rumpuser_kill(int64_t pid, int sig)
{

	TRACE("pid %ld sig %d\n", (long)pid, sig);
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

/*
 * For src/kern/atomic_m68k.c: a section no other task and no interrupt
 * can enter (Disable(), exec.doc), for atomic operations on misaligned
 * words.
 */
void
rumpuser_amiga_atomic_begin(void)
{

	Disable();
}

void
rumpuser_amiga_atomic_end(void)
{

	Enable();
}

/* ------------------------------------------------------------------------
 * threads
 */

/*
 * A new process learns its amthread from a startup message, the way
 * Workbench starts a program (a struct WBStartup, a struct Message
 * first, received on the process's pr_MsgPort: workbench/startup.h).
 * CreateNewProc() (dos.doc) has no tag that hands the process a pointer
 * (dos/dostags.h has no NP_UserData), and the initial tc_UserData of a
 * new process is not documented.  The creator puts the message on
 * &proc->pr_MsgPort; AmiBSDNet sends nothing else there, and the new
 * process takes it before it makes any DOS call.  It answers with the
 * result of its set-up; the message lives on the creator's stack, and
 * the creator waits for that answer.
 */
static void
thread_entry(void)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	struct startmsg *sm;
	struct amthread *t;
	int err;

	WaitPort(&me->pr_MsgPort);
	sm = (struct startmsg *)GetMsg(&me->pr_MsgPort);
	t = sm->t;
	me->pr_Task.tc_UserData = t;

	/* a network stack must never put up DOS requesters */
	me->pr_WindowPtr = (APTR)-1;
	if (amiga_rump_debug) {
		crash_install();
		t->crashtrap = 1;
	}
	err = thread_setup_self(t);
	if (err != 0) {
		if (t->crashtrap)
			crash_remove();
		me->pr_Task.tc_UserData = NULL;
		sm->err = err;
		ReplyMsg(&sm->msg);	/* the creator frees t */
		return;
	}
	sm->err = 0;
	ReplyMsg(&sm->msg);		/* (sm is not touched again) */

	if (__builtin_setjmp(t->jmpbuf) == 0)
		t->func(t->arg);
	thread_teardown_self(t);
	if (t->crashtrap)
		crash_remove();

	Forbid();		/* broken only when this process is gone */
	me->pr_Task.tc_UserData = NULL;
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
	struct MsgPort *reply;
	struct startmsg sm;

	TRACE0();
	if ((t = thread_alloc()) == NULL)
		return RUMPUSER_ENOMEM;
	t->func = f;
	t->arg = arg;
	t->joinable = joinable;
	if ((reply = CreateMsgPort()) == NULL) {
		FreeVec(t);
		return RUMPUSER_EAGAIN;
	}

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
		DeleteMsgPort(reply);
		FreeVec(t);
		return RUMPUSER_EAGAIN;
	}
	memset(&sm, 0, sizeof(sm));
	sm.msg.mn_Node.ln_Type = NT_MESSAGE;
	sm.msg.mn_ReplyPort = reply;
	sm.msg.mn_Length = sizeof(sm);
	sm.t = t;
	PutMsg(&proc->pr_MsgPort, &sm.msg);
	/* (the set-up waited for needs no rump CPU) */
	WaitPort(reply);
	GetMsg(reply);
	DeleteMsgPort(reply);
	if (sm.err != 0) {
		FreeVec(t);
		return sm.err;
	}
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

/* nobody will join a joinable thread after all (as pthread_detach()):
   its state is freed when it ends, or now if it has */
void
amiga_host_thread_detach(void *ptcookie)
{
	struct amthread *t = ptcookie;

	Forbid();
	if (t->done)
		FreeVec(t);
	else
		t->joinable = 0;
	Permit();
}

/* whether a joinable thread has ended */
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
 *
 * librumpuser makes them PTHREAD_MUTEX_ERRORCHECK (rumpuser_pth.c:151)
 * and ends the kernel on any error (NOFAIL_ERRNO).  Exec semaphores nest
 * instead, so re-entering one or releasing another task's is checked
 * here and is fatal in the same way (EDEADLK, EPERM).
 */

#define	RUMPUSER_EDEADLK	11

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
	if ((mtx = AllocVec(sizeof(*mtx), MEMF_SHARED | MEMF_CLEAR)) == NULL)
		fatal("mutex_init", RUMPUSER_ENOMEM);
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

static void
mtx_not_mine(struct rumpuser_mtx *mtx)
{

	if (mtx->sem.ss_Owner == SysBase->ThisTask)
		fatal("mutex_enter: already held", RUMPUSER_EDEADLK);
}

void
rumpuser_mutex_enter_nowrap(struct rumpuser_mtx *mtx)
{

	TRACE0();
	mtx_not_mine(mtx);
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
	mtx_not_mine(mtx);
	if (!AttemptSemaphore(&mtx->sem))
		KLOCK_WRAP(ObtainSemaphore(&mtx->sem));
	mtxenter(mtx);
}

int
rumpuser_mutex_tryenter(struct rumpuser_mtx *mtx)
{

	TRACE0();
	/* (an errorcheck mutex held by the caller: EBUSY, not nested) */
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
	if (mtx->sem.ss_Owner != SysBase->ThisTask)
		fatal("mutex_exit: not held", RUMPUSER_EPERM);
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

/*
 * readers is changed with __atomic builtins, which are CAS on m68k: it
 * must be longword aligned.  On a 68060, CAS with a misaligned effective
 * address is one of the instructions the CPU traps to Motorola's
 * software package (68060SP isp.doc,
 * downloads/sources/linux-m68k/isp.doc:46,174), and Emu68 translates a
 * CAS.L whose address has its low two bits set to a sequence that is not
 * atomic (Emu68 src/M68k_LINE0.c:2752-2808, CAS_UNSAFE()).  So it comes
 * first, in a block AllocVec() gives "long word aligned" (exec.doc
 * AllocMem RESULT).
 */
struct rumpuser_rw {
	volatile unsigned int readers __attribute__((__aligned__(4)));
	/* (unsigned)-1 while write-held */
	struct SignalSemaphore sem;
	struct lwp *writer;
	volatile int downgrade;
};
_Static_assert(__builtin_offsetof(struct rumpuser_rw, readers) == 0,
    "rumpuser_rw.readers must be first (longword aligned)");

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
	if ((rw = AllocVec(sizeof(*rw), MEMF_SHARED | MEMF_CLEAR)) == NULL)
		fatal("rw_init", RUMPUSER_ENOMEM);
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
	if ((cv = AllocVec(sizeof(*cv), MEMF_SHARED | MEMF_CLEAR)) == NULL)
		fatal("cv_init", RUMPUSER_ENOMEM);
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

	if (tmo) {
		/* every rump thread has its timer from set-up */
		if ((tr = t->treq) == NULL)
			fatal("cv_timedwait: no timer", RUMPUSER_EINVAL);
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
	if (!make_timeval(sec, nsec, &tv)) {
		/* over already: the shortest wait, then ETIMEDOUT */
		tv.tv_secs = 0;
		tv.tv_micro = 1;
	}

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

/* a millisecond clock (monotonic; wraps after 49 days) */
unsigned long
amiga_host_ms(void)
{
	int64_t sec;
	long nsec;

	clock_mono(&sec, &nsec);
	return (unsigned long)sec * 1000UL + (unsigned long)(nsec / 1000000L);
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

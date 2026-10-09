/*
 * bsdsocket.library: SocketBaseTagList(), socket passing between bases,
 * syslog and host identity.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

#include "sblib.h"

int	rump_amibsdnet_fd_export(int, int, void **);
int	rump_amibsdnet_fd_import(void *, int *);
void	rump_amibsdnet_fd_discard(void *);

/* ------------------------------------------------------------------------
 * SocketBaseTagList()
 */

#define	SBTC_BREAKMASK		1
#define	SBTC_SIGIOMASK		2
#define	SBTC_SIGURGMASK		3
#define	SBTC_SIGEVENTMASK	4
#define	SBTC_ERRNO		6
#define	SBTC_HERRNO		7
#define	SBTC_DTABLESIZE		8
#define	SBTC_LOGSTAT		10
#define	SBTC_LOGTAGPTR		11
#define	SBTC_LOGFACILITY	12
#define	SBTC_LOGMASK		13
#define	SBTC_ERRNOSTRPTR	14
#define	SBTC_HERRNOSTRPTR	15
#define	SBTC_ERRNOBYTEPTR	21
#define	SBTC_ERRNOWORDPTR	22
#define	SBTC_ERRNOLONGPTR	24
#define	SBTC_HERRNOLONGPTR	25
#define	SBTC_RELEASESTRPTR	29
#define	SBTC_HAVE_ROUTING_API	41
#define	SBTC_HAVE_INTERFACE_API	47
#define	SBTC_HAVE_DNS_API	54

static const char *const errstr[] = {
	"Undefined error: 0", "Operation not permitted",
	"No such file or directory", "No such process",
	"Interrupted system call", "Input/output error",
	"Device not configured", "Argument list too long",
	"Exec format error", "Bad file descriptor", "No child processes",
	"Resource deadlock avoided", "Cannot allocate memory",
	"Permission denied", "Bad address", "Block device required",
	"Device busy", "File exists", "Cross-device link",
	"Operation not supported by device", "Not a directory",
	"Is a directory", "Invalid argument", "Too many open files in system",
	"Too many open files", "Inappropriate ioctl for device",
	"Text file busy", "File too large", "No space left on device",
	"Illegal seek", "Read-only file system", "Too many links",
	"Broken pipe", "Numerical argument out of domain",
	"Result too large", "Resource temporarily unavailable",
	"Operation now in progress", "Operation already in progress",
	"Socket operation on non-socket", "Destination address required",
	"Message too long", "Protocol wrong type for socket",
	"Protocol not available", "Protocol not supported",
	"Socket type not supported", "Operation not supported",
	"Protocol family not supported",
	"Address family not supported by protocol family",
	"Address already in use", "Can't assign requested address",
	"Network is down", "Network is unreachable",
	"Network dropped connection on reset",
	"Software caused connection abort", "Connection reset by peer",
	"No buffer space available", "Socket is already connected",
	"Socket is not connected", "Can't send after socket shutdown",
	"Too many references: can't splice", "Operation timed out",
	"Connection refused", "Too many levels of symbolic links",
	"File name too long", "Host is down", "No route to host",
};
#define	NERRSTR	(sizeof(errstr) / sizeof(errstr[0]))

static const char *const herrstr[] = {
	"Resolver Error 0 (no error)", "Unknown host",
	"Host name lookup failure", "Unknown server error",
	"No address associated with name",
};

static const char release[] = "AmiBSDNet bsdsocket.library 4.1 "
    "(NetBSD 11 TCP/IP)";

static ULONG
tag_get(struct SocketBase *sb, int code, ULONG data)
{

	switch (code) {
	case SBTC_BREAKMASK:	return sb->breakmask;
	case SBTC_SIGIOMASK:	return sb->sigiomask;
	case SBTC_SIGURGMASK:	return sb->sigurgmask;
	case SBTC_SIGEVENTMASK:	return sb->sigeventmask;
	case SBTC_ERRNO:	return sb->sb_errno;
	case SBTC_HERRNO:	return sb->sb_herrno;
	case SBTC_DTABLESIZE:	return sb->dtablesize;
	case SBTC_ERRNOSTRPTR:
		return (ULONG)(data < NERRSTR ? errstr[data] :
		    "Unknown error");
	case SBTC_HERRNOSTRPTR:
		return (ULONG)(data < 5 ? herrstr[data] : "Unknown resolver error");
	case SBTC_RELEASESTRPTR: return (ULONG)release;
	case SBTC_HAVE_ROUTING_API:
	case SBTC_HAVE_INTERFACE_API:
	case SBTC_HAVE_DNS_API:
		return FALSE;
	default:		return 0;
	}
}

static int
tag_set(struct SocketBase *sb, int code, ULONG data)
{

	switch (code) {
	case SBTC_BREAKMASK:	sb->breakmask = data; return 0;
	case SBTC_SIGIOMASK:	sb->sigiomask = data; return 0;
	case SBTC_SIGURGMASK:	sb->sigurgmask = data; return 0;
	case SBTC_SIGEVENTMASK:	sb->sigeventmask = data; return 0;
	case SBTC_ERRNO:	sb_set_errno(sb, data); return 0;
	case SBTC_HERRNO:	sb_set_herrno(sb, data); return 0;
	case SBTC_DTABLESIZE:
		if (data < 1 || data > SB_MAXFD)
			return -1;
		sb->dtablesize = data;
		return 0;
	case SBTC_ERRNOBYTEPTR:	sb->errnoptr = (APTR)data; sb->errnosize = 1;
		return 0;
	case SBTC_ERRNOWORDPTR:	sb->errnoptr = (APTR)data; sb->errnosize = 2;
		return 0;
	case SBTC_ERRNOLONGPTR:	sb->errnoptr = (APTR)data; sb->errnosize = 4;
		return 0;
	case SBTC_HERRNOLONGPTR: sb->herrnoptr = (LONG *)data; return 0;
	case SBTC_LOGSTAT:
	case SBTC_LOGTAGPTR:
	case SBTC_LOGFACILITY:
	case SBTC_LOGMASK:
		return 0;	/* accepted; syslog goes to the stack log */
	default:
		return -1;
	}
}

/*
 * Returns 0, or the 1-based index of the first tag that failed.
 */
LONG
sb_SocketBaseTagList(struct SocketBase *sb, struct TagItem *tags)
{
	struct TagItem *t;
	LONG index = 0;

	for (t = tags; t && t->ti_Tag != TAG_DONE; t++) {
		ULONG tag = t->ti_Tag;
		int code;

		index++;
		if (tag == TAG_IGNORE)
			continue;
		if (tag == TAG_MORE) {
			t = (struct TagItem *)t->ti_Data - 1;
			continue;
		}
		if (tag == TAG_SKIP) {
			t += t->ti_Data;
			continue;
		}
		if (!(tag & TAG_USER))
			return index;
		code = SBTM_CODE(tag);
		if (tag & SBTF_SET) {
			ULONG v = (tag & SBTF_REF) ? *(ULONG *)t->ti_Data :
			    t->ti_Data;

			if (tag_set(sb, code, v) != 0)
				return index;
		} else {
			if (tag & SBTF_REF)
				*(ULONG *)t->ti_Data = tag_get(sb, code,
				    *(ULONG *)t->ti_Data);
			else
				t->ti_Data = tag_get(sb, code, t->ti_Data);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * ReleaseSocket() / ObtainSocket() / ReleaseCopyOfSocket()
 */

#define	UNIQUE_ID	(-1)

struct heldsock {
	struct heldsock *next;
	LONG id;
	LONG type;
	void *file;
};

static struct heldsock *held;
static LONG next_id = 0x10000;
static struct SignalSemaphore heldlock;
static int heldlock_init;

static void
held_lock(void)
{

	Forbid();
	if (!heldlock_init) {
		InitSemaphore(&heldlock);
		heldlock_init = 1;
	}
	Permit();
	ObtainSemaphore(&heldlock);
}

struct release_args { LONG fd, id; int move; };

static LONG
srv_release(struct SocketBase *sb, struct release_args *a)
{
	struct heldsock *h;
	LONG err;

	if (!sb_fdok(sb, a->fd))
		return sb_fail(sb, EBADF);
	if ((h = AllocVec(sizeof(*h), MEMF_ANY | MEMF_CLEAR)) == NULL)
		return sb_fail(sb, ENOMEM);
	if ((err = rump_amibsdnet_fd_export(a->fd, a->move, &h->file)) != 0) {
		FreeVec(h);
		return sb_fail(sb, err);
	}
	h->type = sb->fds[a->fd].type;
	if (a->move)
		sb->fds[a->fd].inuse = 0;
	held_lock();
	h->id = a->id == UNIQUE_ID ? next_id++ : a->id;
	h->next = held;
	held = h;
	ReleaseSemaphore(&heldlock);
	return h->id;
}

LONG
sb_ReleaseSocket(struct SocketBase *sb, LONG sock, LONG id)
{
	struct release_args a = { sock, id, 1 };

	return sb_rpc(sb, (sbfn_t)srv_release, &a, 0, NULL);
}

LONG
sb_ReleaseCopyOfSocket(struct SocketBase *sb, LONG sock, LONG id)
{
	struct release_args a = { sock, id, 0 };

	return sb_rpc(sb, (sbfn_t)srv_release, &a, 0, NULL);
}

struct obtain_args { LONG id, domain, type, protocol; };

static LONG
srv_obtain(struct SocketBase *sb, struct obtain_args *a)
{
	struct heldsock *h, **hp;
	LONG on = 1, err;
	int fd;

	held_lock();
	for (hp = &held; (h = *hp) != NULL; hp = &h->next)
		if (h->id == a->id && (a->type == 0 || h->type == a->type))
			break;
	if (h)
		*hp = h->next;
	ReleaseSemaphore(&heldlock);
	if (h == NULL)
		return sb_fail(sb, EWOULDBLOCK);

	if ((err = rump_amibsdnet_fd_import(h->file, &fd)) != 0) {
		held_lock();		/* put it back */
		h->next = held;
		held = h;
		ReleaseSemaphore(&heldlock);
		return sb_fail(sb, err);
	}
	if (fd >= sb->dtablesize || fd >= SB_MAXFD) {
		rump___sysimpl_close(fd);
		FreeVec(h);
		return sb_fail(sb, EMFILE);
	}
	rump___sysimpl_ioctl(fd, FIONBIO, &on);
	memset(&sb->fds[fd], 0, sizeof(sb->fds[fd]));
	sb->fds[fd].inuse = 1;
	sb->fds[fd].type = (UBYTE)h->type;
	FreeVec(h);
	return fd;
}

LONG
sb_ObtainSocket(struct SocketBase *sb, LONG id, LONG domain, LONG type,
    LONG protocol)
{
	struct obtain_args a = { id, domain, type, protocol };

	return sb_rpc(sb, (sbfn_t)srv_obtain, &a, 0, NULL);
}

/* called by the stack at shutdown (from a rump thread) */
void
sb_discard_held(void)
{
	struct heldsock *h;

	held_lock();
	while ((h = held) != NULL) {
		held = h->next;
		rump_amibsdnet_fd_discard(h->file);
		FreeVec(h);
	}
	ReleaseSemaphore(&heldlock);
}

/* ------------------------------------------------------------------------
 * syslog, host id
 */

void
sb_vsyslog(struct SocketBase *sb, LONG pri, STRPTR msg, APTR args)
{

	if (msg == NULL)
		return;
	amiga_rump_printf("syslog<%ld>: ", (long)pri);
	/* Amiga varargs are a LONG array: hand it to the formatter as such */
	amiga_rump_vprintf_longs((const char *)msg, (const LONG *)args);
	amiga_rump_printf("\n");
}

ULONG	netcfg_primary_address(void);

in_addr_t
sb_gethostid(struct SocketBase *sb)
{

	return netcfg_primary_address();
}

/* interface ioctls with Amiga-sized structures: not translated yet */
LONG
sb_ifioctl(struct SocketBase *sb, LONG fd, ULONG req, APTR argp,
    int *handled)
{

	*handled = 0;
	return 0;
}

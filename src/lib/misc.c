/*
 * bsdsocket.library: SocketBaseTagList(), socket passing between bases,
 * syslog and host identity.
 *
 * "the doc" is downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc,
 * "the SDK header" .../netinclude/libraries/bsdsocket.h, "the catalog"
 * .../locale/bsdsocket.cd (Roadshow's own message strings).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "sblib.h"

int	rump_amibsdnet_fd_export(int, int, void **);
int	rump_amibsdnet_fd_import(void *, int *);
void	rump_amibsdnet_fd_discard(void *);

/* ------------------------------------------------------------------------
 * SocketBaseTagList() (codes from the SDK header)
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
#define	SBTC_IOERRNOSTRPTR	16
#define	SBTC_S2ERRNOSTRPTR	17
#define	SBTC_S2WERRNOSTRPTR	18
#define	SBTC_ERRNOBYTEPTR	21
#define	SBTC_ERRNOWORDPTR	22
#define	SBTC_ERRNOLONGPTR	24
#define	SBTC_HERRNOLONGPTR	25
#define	SBTC_RELEASESTRPTR	29
#define	SBTC_NUM_PACKET_FILTER_CHANNELS 40
#define	SBTC_HAVE_ROUTING_API	41
#define	SBTC_UDP_CHECKSUM	42
#define	SBTC_IP_FORWARDING	43
#define	SBTC_IP_DEFAULT_TTL	44
#define	SBTC_ICMP_MASK_REPLY	45
#define	SBTC_ICMP_SEND_REDIRECTS 46
#define	SBTC_HAVE_INTERFACE_API	47
#define	SBTC_ICMP_PROCESS_ECHO	48
#define	SBTC_ICMP_PROCESS_TSTAMP 49
#define	SBTC_HAVE_MONITORING_API 50
#define	SBTC_CAN_SHARE_LIBRARY_BASES 51
#define	SBTC_HAVE_STATUS_API	53
#define	SBTC_HAVE_DNS_API	54
#define	SBTC_LOG_HOOK		55
#define	SBTC_SYSTEM_STATUS	56
#define	SBTC_SIG_ADDRESS_CHANGE_MASK 57
#define	SBTC_HAVE_LOCAL_DATABASE_API 59
#define	SBTC_HAVE_ADDRESS_CONVERSION_API 60
#define	SBTC_HAVE_KERNEL_MEMORY_API 61
#define	SBTC_HAVE_SERVER_API	63
#define	SBTC_GET_BYTES_RECEIVED	64
#define	SBTC_GET_BYTES_SENT	65
/* (downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/netinclude/libraries/
   bsdsocket.h:250) */
#define	SBTC_IDN_DEFAULT_CHARACTER_SET 66
#define	SBTC_HAVE_ROADSHOWDATA_API 67
#define	SBTC_ERROR_HOOK		68
#define	SBTC_HAVE_GETHOSTADDR_R_API 69

/* IR_ and IDNCS_ values (the SDK header) */
#define	IR_Process		0
#define	IDNCS_ASCII		0

/* SBSYSSTAT_ flags (the SDK header) */
#define	SBSYSSTAT_Resolver	(1L << 3)

/*
 * The catalog, MSG_BSDSOCKET_SBT_ERRNO_*: errno 0..79.  Entry 78 (ENOSYS)
 * is left out: a rule of this project rejects the wording of its catalog
 * text in sources, so it is reported like an unknown code.
 */
static const char *const errstr[] = {
	"No error", "Operation not permitted", "Object not found",
	"No such process", "Interrupted system call", "Input/output error",
	"Device not configured", "Argument list too long",
	"Exec format error", "Bad file descriptor", "No child processes",
	"Resource deadlock avoided", "Cannot allocate memory",
	"Permission denied", "Bad address", "Block device required",
	"Device busy", "Object exists", "Cross-device link",
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
	"Address already in use", "Cannot assign requested address",
	"Network is down", "Network is unreachable",
	"Network dropped connection on reset",
	"Software caused connection abort", "Connection reset by peer",
	"No buffer space available", "Socket is already connected",
	"Socket is not connected", "Cannot send after socket shutdown",
	"Too many references: cannot splice", "Operation timed out",
	"Connection refused", "Too many levels of symbolic links",
	"File name too long", "Host is down", "No route to host",
	"Directory not empty", "Too many processes", "Too many users",
	"Disc quota exceeded", "Stale NFS file handle",
	"Too many levels of remote in path", "RPC struct is bad",
	"RPC version wrong", "RPC program not avail", "Program version wrong",
	"Bad procedure for program", "No locks available",
	NULL, "Inappropriate file type or format",
};
#define	NERRSTR	(sizeof(errstr) / sizeof(errstr[0]))

/* MSG_BSDSOCKET_SBT_HERRNO_* */
static const char *const herrstr[] = {
	"No error", "Unknown host", "Host name lookup failure",
	"Unknown server error", "No address associated with name",
};
#define	NHERRSTR	(sizeof(herrstr) / sizeof(herrstr[0]))

static const char release[] = "AmiBSDNet bsdsocket.library 4.1 "
    "(NetBSD 11 TCP/IP)";

/* errno text, or the catalog's "Unknown error: %ld" in the base's buffer */
const char *
sb_strerror(struct SocketBase *sb, LONG e)
{
	char *p;

	if (e >= 0 && (ULONG)e < NERRSTR && errstr[e])
		return errstr[e];
	p = sb->errbuf;
	sb_strlcpy(p, "Unknown error: ", sizeof(sb->errbuf));
	p += sb_strlen(p);
	if (e < 0) {
		*p++ = '-';
		e = -e;
	}
	*sb_fmt_ulong(p, (ULONG)e) = '\0';
	return sb->errbuf;
}

/*
 * SBTC_IOERRNOSTRPTR, SBTC_S2ERRNOSTRPTR, SBTC_S2WERRNOSTRPTR: the texts of
 * the catalog (downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/locale/
 * bsdsocket.cd:523-655), indexed by the code: an Exec I/O error as a
 * positive number (the doc, "a pointer to a positive integer"; IOERR_*
 * in exec/errors.h are -1 to -7), S2ERR_* and S2WERR_* as in
 * devices/sana2.h:350-394.  A code without a text gets the catalog's
 * "Unknown ... %ld" in the base's buffer.
 */
static const char *const ioerrstr[] = {
	"No error", "Device/unit failed to open", "Request terminated early",
	"Command not supported by device", "Not a valid length",
	"Invalid address", "Device opens OK, but requested unit is busy",
	"Hardware failed self-test"
};
static const char *const s2errstr[] = {
	"No error", "Resource allocation failure", NULL, "Bad argument",
	"Inappropriate state", "Bad address",
	"Maximum transmission unit exceeded", NULL,
	"Command not supported by hardware", "Software error detected",
	"Driver is offline", "Transmission attempt failed"
};
static const char *const s2werrstr[] = {
	"No specific information available", "Unit not configured",
	"Unit is currently online", "Unit is currently offline",
	"Protocol already tracked", "Protocol not tracked",
	"Buffer management function returned error",
	"Source address problem", "Destination address problem",
	"Broadcast address problem", "Multicast address problem",
	"Multicast address list is full", "Unsupported event class",
	"StatData failed sanity check", NULL, "Attempt to configure twice",
	"NULL pointer detected", "Transmission failed - too many retries",
	"Driver fixable hardware error",
	/* (19-23: bsdsocket.cd:641-655, devices/sana2.h:395-399) */
	"Unit currently not connected", "Unit currently connected",
	"Invalid option rejected", "Mandatory option missing",
	"Could not log in"
};

static const char *
code_str(struct SocketBase *sb, const char *const *tab, ULONG n, LONG e,
    const char *unknown)
{
	char *p;

	if (e >= 0 && (ULONG)e < n && tab[e])
		return tab[e];
	p = sb->errbuf;
	sb_strlcpy(p, unknown, sizeof(sb->errbuf));
	p += sb_strlen(p);
	if (e < 0) {
		*p++ = '-';
		e = -e;
	}
	*sb_fmt_ulong(p, (ULONG)e) = '\0';
	return sb->errbuf;
}

#define	NTAB(t)	(sizeof(t) / sizeof((t)[0]))

/* the tags whose values come from the kernel (server side) */
struct tag_args { int code, set; ULONG v; ULONG *ref; };

static LONG
srv_tag(struct SocketBase *sb, struct tag_args *a)
{
	ULONG f;
	int e;

	switch (a->code) {
	case SBTC_SYSTEM_STATUS:
		/* (if_status_flags() returns the error itself) */
		if ((e = if_status_flags(&f)) != 0)
			return sb_fail(sb, e);
		/* "Domain name server addresses are configured" */
		if (netdb_nameserver_count() > 0)
			f |= SBSYSSTAT_Resolver;
		/* "A default route is configured"; routes (route.c) */
		f |= route_status();
		a->v = f;
		return 0;
	case SBTC_GET_BYTES_RECEIVED:
	case SBTC_GET_BYTES_SENT:
	    {
		ULONG ih, il, oh, ol;
		int e;

		if ((e = nb_bytes_total(&ih, &il, &oh, &ol)) != 0)
			return sb_fail(sb, e);
		/* SBQUAD_T: sbq_High, sbq_Low (the SDK header) */
		a->ref[0] = a->code == SBTC_GET_BYTES_RECEIVED ? ih : oh;
		a->ref[1] = a->code == SBTC_GET_BYTES_RECEIVED ? il : ol;
		return 0;
	    }
	case SBTC_SIGIOMASK:
	case SBTC_SIGURGMASK:
	case SBTC_SIGEVENTMASK:
	case SBTC_SIG_ADDRESS_CHANGE_MASK:
		return ev_sync(sb, NULL);
	}
	if (a->set)
		return nb_tag_set(a->code, a->v) == 0 ? 0 :
		    sb_fail(sb, sb_rumperr());
	return nb_tag_get(a->code, &a->v) == 0 ? 0 :
	    sb_fail(sb, sb_rumperr());
}

static int
kernel_tag(struct SocketBase *sb, int code, int set, ULONG *v, ULONG *ref)
{
	struct tag_args a;

	a.code = code;
	a.set = set;
	a.v = *v;
	a.ref = ref;
	if (sb_rpc(sb, (sbfn_t)srv_tag, &a, 0, NULL) != 0)
		return -1;
	*v = a.v;
	return 0;
}

/*
 * SBTC_LOG_HOOK: one for the whole stack.  It points into the program
 * that set it, so it goes when that program's base is closed
 * (log_base_closed()), and a hook that is replaced or goes is not given
 * back before the calls of it by other tasks are over.  Under loglock,
 * which is never held while a hook runs.
 */
struct Hook *sb_log_hook;
static struct SocketBase *log_hook_owner;
static struct SignalSemaphore loglock;

/* a call of the log hook under way (on the caller's stack) */
struct logcall {
	struct logcall *next;
	struct Task *task;
	struct Hook *hook;
};
static struct logcall *logcalls;

/* sb_log_hook = hook; then wait for the calls of the old one by other
   tasks (the calling task's own are the ones it is in) */
static void
log_hook_set(struct SocketBase *sb, struct Hook *hook, int onlyowner)
{
	struct Hook *old;
	struct Task *me = FindTask(NULL);
	struct logcall *c;
	int busy;

	ObtainSemaphore(&loglock);
	if (onlyowner && log_hook_owner != sb) {
		ReleaseSemaphore(&loglock);
		return;
	}
	old = sb_log_hook;
	sb_log_hook = hook;
	log_hook_owner = hook ? sb : NULL;
	ReleaseSemaphore(&loglock);
	if (old == NULL || old == hook)
		return;
	do {
		ObtainSemaphore(&loglock);
		for (busy = 0, c = logcalls; c; c = c->next)
			if (c->hook == old && c->task != me)
				busy = 1;
		ReleaseSemaphore(&loglock);
		if (busy)
			Delay(1);
	} while (busy);
}

/* the base goes (CloseLibrary): its log hook goes too */
void
log_base_closed(struct SocketBase *sb)
{

	log_hook_set(sb, NULL, 1);
}

/* returns 0 and the value, or -1 for a code that cannot be read */
static int
tag_get(struct SocketBase *sb, int code, ULONG data, ULONG *v, ULONG *ref)
{

	switch (code) {
	case SBTC_BREAKMASK:	*v = sb->breakmask; return 0;
	case SBTC_SIGIOMASK:	*v = sb->sigiomask; return 0;
	case SBTC_SIGURGMASK:	*v = sb->sigurgmask; return 0;
	case SBTC_SIGEVENTMASK:	*v = sb->sigeventmask; return 0;
	case SBTC_SIG_ADDRESS_CHANGE_MASK: *v = sb->sigaddrmask; return 0;
	case SBTC_ERRNO:	*v = sb->sb_errno; return 0;
	case SBTC_HERRNO:	*v = sb->sb_herrno; return 0;
	case SBTC_DTABLESIZE:	*v = sb->dtablesize; return 0;
	case SBTC_LOGSTAT:	*v = sb->logstat; return 0;
	case SBTC_LOGTAGPTR:	*v = (ULONG)sb->logtag; return 0;
	case SBTC_LOGFACILITY:	*v = sb->logfacility; return 0;
	case SBTC_LOGMASK:	*v = sb->logmask; return 0;
	case SBTC_ERRNOSTRPTR:
		*v = (ULONG)sb_strerror(sb, (LONG)data);
		return 0;
	case SBTC_HERRNOSTRPTR:
		*v = (ULONG)(data < NHERRSTR ? herrstr[data] :
		    "Unknown resolver error");
		return 0;
	case SBTC_IOERRNOSTRPTR:
		*v = (ULONG)code_str(sb, ioerrstr, NTAB(ioerrstr), (LONG)data,
		    "Unknown I/O error ");
		return 0;
	case SBTC_S2ERRNOSTRPTR:
		*v = (ULONG)code_str(sb, s2errstr, NTAB(s2errstr), (LONG)data,
		    "Unknown SANA-II error ");
		return 0;
	case SBTC_S2WERRNOSTRPTR:
		*v = (ULONG)code_str(sb, s2werrstr, NTAB(s2werrstr),
		    (LONG)data, "Unknown SANA-II wire error ");
		return 0;
	/* the kernel answers the ICMP echo and time stamp requests sent to
	   this host (netbsd-src/sys/netinet/ip_icmp.c:549-570; broadcast and
	   multicast ones not, as net.inet.icmp.bmcastecho is 0, line 142),
	   and this library does no domain name translation: that is what
	   is reported (bsdsocket.h:270,324 IDNCS_ASCII, IR_Process) */
	case SBTC_ICMP_PROCESS_ECHO:
	case SBTC_ICMP_PROCESS_TSTAMP:
		*v = IR_Process;
		return 0;
	case SBTC_IDN_DEFAULT_CHARACTER_SET: *v = IDNCS_ASCII; return 0;
	case SBTC_RELEASESTRPTR: *v = (ULONG)release; return 0;
	case SBTC_ERROR_HOOK:	*v = (ULONG)sb->errorhook; return 0;
	case SBTC_CAN_SHARE_LIBRARY_BASES: *v = sb->share; return 0;
	case SBTC_LOG_HOOK:	*v = (ULONG)sb_log_hook; return 0;
	/* the packet filter channels of bpf.c */
	case SBTC_NUM_PACKET_FILTER_CHANNELS: *v = SB_BPF_CHANNELS; return 0;
	/* the APIs this library provides */
	case SBTC_HAVE_ROUTING_API:
	case SBTC_HAVE_INTERFACE_API:
	case SBTC_HAVE_MONITORING_API:
	case SBTC_HAVE_STATUS_API:
	case SBTC_HAVE_DNS_API:
	case SBTC_HAVE_LOCAL_DATABASE_API:
	case SBTC_HAVE_ADDRESS_CONVERSION_API:
	case SBTC_HAVE_KERNEL_MEMORY_API:
	case SBTC_HAVE_SERVER_API:
	case SBTC_HAVE_ROADSHOWDATA_API:
	case SBTC_HAVE_GETHOSTADDR_R_API:
		*v = TRUE;
		return 0;
	case SBTC_UDP_CHECKSUM:
	case SBTC_IP_FORWARDING:
	case SBTC_IP_DEFAULT_TTL:
	case SBTC_ICMP_MASK_REPLY:
	case SBTC_ICMP_SEND_REDIRECTS:
	case SBTC_SYSTEM_STATUS:
		return kernel_tag(sb, code, 0, v, NULL);
	case SBTC_GET_BYTES_RECEIVED:
	case SBTC_GET_BYTES_SENT:
		/* "Note that the data must be passed by reference" */
		if (ref == NULL)
			return -1;
		return kernel_tag(sb, code, 0, v, ref);
	}
	return -1;
}

/* returns 0, or -1 for a code that cannot be set (or a bad value) */
static int
tag_set(struct SocketBase *sb, int code, ULONG data)
{
	int fd;
	ULONG *mask, old;

	switch (code) {
	case SBTC_BREAKMASK:	sb->breakmask = data; return 0;
	case SBTC_SIGIOMASK:
	case SBTC_SIGURGMASK:
	case SBTC_SIGEVENTMASK:
	case SBTC_SIG_ADDRESS_CHANGE_MASK:
		if (code == SBTC_SIGIOMASK)
			mask = &sb->sigiomask;
		else if (code == SBTC_SIGURGMASK)
			mask = &sb->sigurgmask;
		else if (code == SBTC_SIGEVENTMASK)
			mask = &sb->sigeventmask;
		else
			mask = &sb->sigaddrmask;
		old = *mask;
		*mask = data;
		/* the event thread looks at the new signals; if it cannot
		   (it does not start), the tag fails and nothing changed */
		if (kernel_tag(sb, code, 1, &data, NULL) != 0) {
			*mask = old;
			return -1;
		}
		return 0;
	case SBTC_ERRNO:	sb_set_errno(sb, data); return 0;
	case SBTC_HERRNO:	sb_set_herrno(sb, data); return 0;
	case SBTC_DTABLESIZE:
		/* the table cannot shrink below a socket in use */
		if (data < 1 || data > SB_MAXFD)
			return -1;
		for (fd = data; fd < SB_MAXFD; fd++)
			if (sb->fds[fd].inuse)
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
	case SBTC_LOGSTAT:	sb->logstat = data; return 0;
	case SBTC_LOGTAGPTR:	sb->logtag = (STRPTR)data; return 0;
	case SBTC_LOGFACILITY:	sb->logfacility = data; return 0;
	case SBTC_LOGMASK:	sb->logmask = data; return 0;
	case SBTC_ERROR_HOOK:	sb->errorhook = (struct Hook *)data; return 0;
	case SBTC_CAN_SHARE_LIBRARY_BASES: sb->share = data != 0; return 0;
	case SBTC_LOG_HOOK:
		log_hook_set(sb, (struct Hook *)data, 0);
		return 0;
	case SBTC_UDP_CHECKSUM:
	case SBTC_IP_FORWARDING:
	case SBTC_IP_DEFAULT_TTL:
	case SBTC_ICMP_MASK_REPLY:
	case SBTC_ICMP_SEND_REDIRECTS:
		return kernel_tag(sb, code, 1, &data, NULL);
	/* (only what tag_get() reports can be set: the kernel has no
	   setting to ignore or drop these requests) */
	case SBTC_ICMP_PROCESS_ECHO:
	case SBTC_ICMP_PROCESS_TSTAMP:
		return data == IR_Process ? 0 : -1;
	/* "if the library does not understand the character set code you
	   select, then it will behave as if IDNCS_ASCII had been selected"
	   (bsdsocket.doc:9739-9741): it understands none, so any is taken */
	case SBTC_IDN_DEFAULT_CHARACTER_SET:
		return 0;
	}
	return -1;
}

/*
 * The doc: "Returns 0 on success, and a (positive) index of the failing
 * tag on error".
 */
LONG
sb_SocketBaseTagList(struct SocketBase *sb, struct TagItem *tags)
{
	struct TagItem *tstate = tags, *t;
	LONG index = 0;

	while ((t = sb_next_tag(&tstate)) != NULL) {
		ULONG tag = t->ti_Tag, v;
		int code;

		index++;
		if (!(tag & TAG_USER))
			return index;
		code = SBTM_CODE(tag);
		if (tag & SBTF_SET) {
			if ((tag & SBTF_REF) && t->ti_Data == 0)
				return index;
			v = (tag & SBTF_REF) ? *(ULONG *)t->ti_Data :
			    t->ti_Data;
			if (tag_set(sb, code, v) != 0)
				return index;
		} else if (tag & SBTF_REF) {
			ULONG *p = (ULONG *)t->ti_Data;

			if (p == NULL || tag_get(sb, code, *p, &v, p) != 0)
				return index;
			/* (the 64-bit counters were stored through p) */
			if (code != SBTC_GET_BYTES_RECEIVED &&
			    code != SBTC_GET_BYTES_SENT)
				*p = v;
		} else {
			if (tag_get(sb, code, t->ti_Data, &v, NULL) != 0)
				return index;
			t->ti_Data = v;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * ReleaseSocket() / ObtainSocket() / ReleaseCopyOfSocket()
 */

#define	UNIQUE_ID	(-1)		/* the SDK header */

struct heldsock {
	struct heldsock *next;
	LONG id;
	LONG domain, type, protocol;
	void *file;
	/* the library's own state of the socket (sbfd), which the kernel
	   file does not carry */
	UBYTE nonblock, listening, has_rcvto, has_sndto;
	struct sbtime rcvto, sndto;
};

static struct heldsock *held;
static LONG next_id = 0x10000;
static struct SignalSemaphore heldlock;

void
held_init(void)
{

	InitSemaphore(&heldlock);
	InitSemaphore(&loglock);
}

/* socket()'s protocol 0 is the type's protocol: "Normally only a single
   protocol exists to support a particular socket type" (the doc,
   socket) */
static LONG
proto_of(LONG domain, LONG type, LONG protocol)
{

	if (protocol == 0 && (domain == AF_INET || domain == AF_INET6)) {
		if (type == SOCK_STREAM)
			return IPPROTO_TCP;
		if (type == SOCK_DGRAM)
			return IPPROTO_UDP;
	}
	return protocol;
}

/* is an id in use? (heldlock held) */
static int
id_held(LONG id)
{
	struct heldsock *h;

	for (h = held; h; h = h->next)
		if (h->id == id)
			return 1;
	return 0;
}

struct release_args { LONG fd, id; int move; };

static LONG
srv_release(struct SocketBase *sb, struct release_args *a)
{
	struct heldsock *h, **hp;
	LONG err, id;
	void *file;

	if (!sb_fdok(sb, a->fd))
		return sb_fail(sb, EBADF);
	/* the doc: an id "between 0 and 65535 (inclusively)" is
	   non-unique, a greater one "must be unique", and UNIQUE_ID asks
	   for one; nothing else is an id */
	if (a->id < 0 && a->id != UNIQUE_ID)
		return sb_fail(sb, EINVAL);
	if ((h = AllocVec(sizeof(*h), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return sb_fail(sb, ENOMEM);
	ObtainSemaphore(&heldlock);
	if (a->id > 65535 && id_held(a->id)) {
		ReleaseSemaphore(&heldlock);
		FreeVec(h);
		return sb_fail(sb, EINVAL);	/* "not unique" */
	}
	if (a->id == UNIQUE_ID) {
		/* a fresh one, not used by anyone (also not chosen by a
		   program as its own unique id) */
		do {
			if (next_id <= 65535)
				next_id = 0x10000;
			h->id = next_id++;
		} while (id_held(h->id));
	} else
		h->id = a->id;
	h->domain = sb->fds[a->fd].domain;
	h->type = sb->fds[a->fd].type;
	h->protocol = proto_of(h->domain, h->type, sb->fds[a->fd].protocol);
	h->nonblock = sb->fds[a->fd].nonblock;
	h->listening = sb->fds[a->fd].listening;
	h->has_rcvto = sb->fds[a->fd].has_rcvto;
	h->has_sndto = sb->fds[a->fd].has_sndto;
	h->rcvto = sb->fds[a->fd].rcvto;
	h->sndto = sb->fds[a->fd].sndto;
	/* in the list at once (without a file it cannot be obtained), so
	   the id stays reserved */
	h->next = held;
	held = h;
	ReleaseSemaphore(&heldlock);
	/* the slot is given up before the kernel frees the descriptor
	   number (fd_close() in rump_amibsdnet_fd_export()): a task sharing
	   this base may get that number at once (socket(), accept(), the
	   import in srv_obtain()), and its slot must not be cleared after
	   that.  If the export fails the descriptor is gone as well
	   (src/kern/sockpass.c: EBADF, or fd_close() failing because it is
	   being closed) or is not a socket, which no slot of the table is */
	if (a->move) {
		ev_fd_closed(sb, a->fd);
		ObtainSemaphore(&sb->evlock);
		sb->fds[a->fd].inuse = 0;
		ReleaseSemaphore(&sb->evlock);
	}
	if ((err = rump_amibsdnet_fd_export(a->fd, a->move, &file)) != 0) {
		ObtainSemaphore(&heldlock);
		for (hp = &held; *hp && *hp != h; hp = &(*hp)->next)
			;
		if (*hp)
			*hp = h->next;
		ReleaseSemaphore(&heldlock);
		FreeVec(h);
		return sb_fail(sb, err);
	}
	/* h is not touched after it has its file: srv_obtain() frees it */
	id = h->id;
	ObtainSemaphore(&heldlock);
	h->file = file;
	ReleaseSemaphore(&heldlock);
	return id;
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

	/* "The socket to be handed over must be identified by an ID, a
	   domain, type and protocol number" */
	ObtainSemaphore(&heldlock);
	for (hp = &held; (h = *hp) != NULL; hp = &h->next)
		if (h->file && h->id == a->id && h->domain == a->domain &&
		    h->type == a->type &&
		    h->protocol == proto_of(a->domain, a->type, a->protocol))
			break;
	if (h)
		*hp = h->next;
	ReleaseSemaphore(&heldlock);
	/* "[EBADF] No socket with the given Id could be found" */
	if (h == NULL)
		return sb_fail(sb, EBADF);

	if ((err = rump_amibsdnet_fd_import(h->file, &fd)) != 0) {
		ObtainSemaphore(&heldlock);	/* put it back */
		h->next = held;
		held = h;
		ReleaseSemaphore(&heldlock);
		return sb_fail(sb, err);
	}
	if (fd >= sb->dtablesize || fd >= SB_MAXFD) {
		/* no room in this base: hand the socket back, still waiting
		   to be obtained; if even that fails it is closed rather
		   than left in this process unaccounted for */
		if (rump_amibsdnet_fd_export(fd, 1, &h->file) == 0) {
			ObtainSemaphore(&heldlock);
			h->next = held;
			held = h;
			ReleaseSemaphore(&heldlock);
		} else {
			rump___sysimpl_close(fd);
			FreeVec(h);
		}
		return sb_fail(sb, EMFILE);
	}
	rump___sysimpl_ioctl(fd, FIONBIO, &on);
	ev_fd_closed(sb, fd);
	ObtainSemaphore(&sb->evlock);
	memset(&sb->fds[fd], 0, sizeof(sb->fds[fd]));
	sb->fds[fd].domain = (UBYTE)h->domain;
	sb->fds[fd].type = (UBYTE)h->type;
	sb->fds[fd].protocol = h->protocol;
	sb->fds[fd].route = h->domain == AF_ROUTE;
	sb->fds[fd].nonblock = h->nonblock;
	sb->fds[fd].listening = h->listening;
	sb->fds[fd].has_rcvto = h->has_rcvto;
	sb->fds[fd].has_sndto = h->has_sndto;
	sb->fds[fd].rcvto = h->rcvto;
	sb->fds[fd].sndto = h->sndto;
	sb->fds[fd].inuse = 1;
	ReleaseSemaphore(&sb->evlock);
	FreeVec(h);
	if (sb->sigurgmask)
		ev_note(sb, fd, 0, SIGIO_URG);
	return fd;
}

LONG
sb_ObtainSocket(struct SocketBase *sb, LONG id, LONG domain, LONG type,
    LONG protocol)
{
	struct obtain_args a = { id, domain, type, protocol };

	return sb_rpc(sb, (sbfn_t)srv_obtain, &a, 0, NULL);
}

/* ------------------------------------------------------------------------
 * syslog
 */

/* the SDK header: struct LogHookMessage */
struct LogHookMessage {
	LONG lhm_Size;
	LONG lhm_Priority;
	struct DateStamp lhm_Date;
	STRPTR lhm_Tag;
	ULONG lhm_ID;
	STRPTR lhm_Message;
};

/* netbsd-src/sys/sys/syslog.h */
#define	LOG_PRIMASK	0x07
#define	LOG_FACMASK	0x03f8
#define	LOG_MASK(pri)	(1 << (pri))

/* 64-bit unsigned (hi:lo) to decimal, without 64-bit division helpers */
static char *
fmt_u64(char *p, ULONG hi, ULONG lo)
{
	char tmp[24];
	int i = 0;

	do {
		/* (hi:lo) / 10 with 16-bit pieces */
		ULONG r, q3, q2, q1, q0, w;

		w = hi >> 16;		q3 = w / 10; r = w % 10;
		w = (r << 16) | (hi & 0xffff); q2 = w / 10; r = w % 10;
		w = (r << 16) | (lo >> 16); q1 = w / 10; r = w % 10;
		w = (r << 16) | (lo & 0xffff); q0 = w / 10; r = w % 10;
		tmp[i++] = '0' + r;
		hi = (q3 << 16) | q2;
		lo = (q1 << 16) | q0;
	} while (hi || lo);
	while (i)
		*p++ = tmp[--i];
	return p;
}

/*
 * A printf() format with arguments in a stack argument array (every
 * integer argument one LONG, a long long two), into buf; %m is the text
 * of the base's errno (the doc, vsyslog: "`%m' is replaced by the current
 * error message").  Returns the length.
 */
int
sb_format(char *buf, int size, const char *fmt, const LONG *args,
    struct SocketBase *sb)
{
	char *o = buf, *end = buf + size - 1;
	LONG saved_errno = sb ? sb->sb_errno : 0;

#define	PUT(c)	do { if (o < end) *o++ = (c); } while (0)
	for (; *fmt; fmt++) {
		char num[32], *s;
		const char *str;
		int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
		int width = 0, prec = -1, ll = 0, len, i, neg = 0;
		ULONG u = 0, uh = 0;
		char conv;

		if (*fmt != '%') {
			PUT(*fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-') left = 1;
			else if (*fmt == '0') zero = 1;
			else if (*fmt == '+') plus = 1;
			else if (*fmt == ' ') space = 1;
			else if (*fmt == '#') alt = 1;
			else break;
		}
		if (*fmt == '*') {
			width = (int)*args++;
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
				prec = (int)*args++;
				fmt++;
			} else
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
		}
		while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' ||
		    *fmt == 'q') {
			if (*fmt == 'q' || (fmt[0] == 'l' && fmt[1] == 'l'))
				ll = 1;
			if (fmt[0] == 'l' && fmt[1] == 'l')
				fmt++;
			fmt++;
		}
		conv = *fmt;
		if (conv == '\0')
			break;
		s = num;
		str = NULL;
		switch (conv) {
		case 'm':
			str = sb ? sb_strerror(sb, saved_errno) : "";
			break;
		case 's':
			str = (const char *)*args++;
			if (str == NULL)
				str = "(null)";
			break;
		case 'c':
			num[0] = (char)*args++;
			num[1] = '\0';
			str = num;
			break;
		case '%':
			str = "%";
			break;
		case 'd': case 'i':
			if (ll) {
				uh = (ULONG)*args++;
				u = (ULONG)*args++;
				if ((LONG)uh < 0) {
					neg = 1;
					uh = ~uh;
					u = -u;
					if (u == 0)
						uh++;
				}
			} else {
				LONG v = *args++;

				if (v < 0)
					neg = 1, u = -(ULONG)v;
				else
					u = v;
			}
			s = fmt_u64(num, uh, u);
			*s = '\0';
			break;
		case 'u':
			if (ll)
				uh = (ULONG)*args++;
			u = (ULONG)*args++;
			*fmt_u64(num, uh, u) = '\0';
			break;
		case 'x': case 'X': case 'p': case 'o':
		    {
			int sh = conv == 'o' ? 3 : 4, n = 0;
			const char *dig = conv == 'X' ? "0123456789ABCDEF" :
			    "0123456789abcdef";
			char tmp[24];

			if (ll)
				uh = (ULONG)*args++;
			u = (ULONG)*args++;
			if (conv == 'p')
				alt = 1;
			do {
				tmp[n++] = dig[u & ((1 << sh) - 1)];
				u = (u >> sh) | (uh << (32 - sh));
				uh >>= sh;
			} while (u || uh);
			for (i = 0; i < n; i++)
				num[i] = tmp[n - 1 - i];
			num[n] = '\0';
			break;
		    }
		default:
			/* not a conversion this formatter knows: as text */
			PUT('%');
			PUT(conv);
			continue;
		}
		if (str == NULL) {
			/* a number in num */
			const char *pfx = neg ? "-" : plus && (conv == 'd' ||
			    conv == 'i') ? "+" : space && (conv == 'd' ||
			    conv == 'i') ? " " : (alt && (conv == 'x' ||
			    conv == 'p')) ? "0x" : (alt && conv == 'X') ? "0X" :
			    (alt && conv == 'o') ? "0" : "";
			int nl = sb_strlen(num), pl = sb_strlen(pfx), zeros = 0;

			if (prec >= 0 && prec > nl)
				zeros = prec - nl;
			else if (zero && !left && prec < 0 &&
			    width > nl + pl)
				zeros = width - nl - pl;
			len = pl + zeros + nl;
			if (!left)
				for (i = len; i < width; i++)
					PUT(' ');
			for (i = 0; i < pl; i++)
				PUT(pfx[i]);
			for (i = 0; i < zeros; i++)
				PUT('0');
			for (i = 0; i < nl; i++)
				PUT(num[i]);
			if (left)
				for (i = len; i < width; i++)
					PUT(' ');
			continue;
		}
		len = sb_strlen(str);
		if (prec >= 0 && prec < len)
			len = prec;
		if (!left)
			for (i = len; i < width; i++)
				PUT(' ');
		for (i = 0; i < len; i++)
			PUT(str[i]);
		if (left)
			for (i = len; i < width; i++)
				PUT(' ');
	}
#undef PUT
	*o = '\0';
	return o - buf;
}

/*
 * vsyslog(): the doc, "`%m' is replaced by the current error message"
 * and "A trailing newline is added if none is present"; SBTC_LOGMASK
 * filters by priority (LOG_MASK(), netbsd-src/sys/sys/syslog.h); a
 * priority without a facility gets SBTC_LOGFACILITY; SBTC_LOGTAGPTR
 * marks the message; with SBTC_LOG_HOOK set the hook gets the message
 * "rather than sending log messages to the process which records and
 * displays them" (here: the stack's log).
 */
void
sb_vsyslog(struct SocketBase *sb, LONG pri, STRPTR msg, APTR args)
{
	char *buf, *p, *text;
	int n, room;
	const char *tag = (const char *)sb->logtag;
	struct logcall c;

	if (msg == NULL)
		return;
	if (!(LOG_MASK(pri & LOG_PRIMASK) & sb->logmask))
		return;
	if ((pri & LOG_FACMASK) == 0)
		pri |= sb->logfacility & LOG_FACMASK;
	if ((buf = AllocVec(1024, MEMF_PUBLIC)) == NULL)
		return;
	p = buf;
	*p++ = '<';
	p = sb_fmt_ulong(p, (ULONG)pri);
	*p++ = '>';
	*p = '\0';
	if (tag && *tag) {
		sb_strlcpy(p, tag, 1024 - 8 - (p - buf));
		p += sb_strlen(p);
		*p++ = ':';
		*p++ = ' ';
		*p = '\0';
	}
	room = 1024 - 2 - (p - buf);
	text = p;
	n = sb_format(p, room, (const char *)msg, (const LONG *)args, sb);
	p += n;
	if (p == buf || p[-1] != '\n')
		*p++ = '\n';
	*p = '\0';
	ObtainSemaphore(&loglock);
	c.hook = UtilityBase ? sb_log_hook : NULL;
	if (c.hook) {
		c.task = FindTask(NULL);
		c.next = logcalls;
		logcalls = &c;
	}
	ReleaseSemaphore(&loglock);
	if (c.hook) {
		struct LogHookMessage m;
		struct logcall **cp;

		m.lhm_Size = sizeof(m);
		m.lhm_Priority = pri & LOG_PRIMASK;
		DateStamp(&m.lhm_Date);
		m.lhm_Tag = (STRPTR)tag;
		m.lhm_ID = 0;
		/* "the log message to be displayed" (netinclude/libraries/
		   bsdsocket.h:314): the formatted text only, without the
		   "<priority>tag: " and the line end the log line has; the
		   priority and the tag are fields of their own */
		if (p > text && p[-1] == '\n')
			p[-1] = '\0';
		m.lhm_Message = (STRPTR)text;
		CallHookPkt(c.hook, NULL, &m);
		ObtainSemaphore(&loglock);
		for (cp = &logcalls; *cp && *cp != &c; cp = &(*cp)->next)
			;
		if (*cp)
			*cp = c.next;
		ReleaseSemaphore(&loglock);
	} else
		amiga_rump_printf("%s", buf);
	FreeVec(buf);
}

/* ------------------------------------------------------------------------
 * host id
 */

ULONG	netcfg_primary_address(void);

in_addr_t
sb_gethostid(struct SocketBase *sb)
{
	ULONG a;

	/* (the stack's interface table, read in one piece) */
	Forbid();
	a = netcfg_primary_address();
	Permit();
	return a;
}

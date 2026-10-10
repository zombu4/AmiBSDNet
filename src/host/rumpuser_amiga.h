/*
 * Host (AmigaOS) side of the rump kernel: the rumpuser(3) hypercall
 * interface implemented on Exec/DOS, plus a few helpers for the code that
 * drives the rump kernel.
 *
 * Host code is compiled with the Amiga NDK headers and is *not* put
 * through the rumpns_ symbol rename, so it must only talk to the kernel
 * through rump_*() entry points and rumpuser_*() hypercalls.
 */
#ifndef RUMPUSER_AMIGA_H
#define RUMPUSER_AMIGA_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* just enough of <sys/cdefs.h> and <sys/types.h> for rump/rumpuser.h */
#define LIBRUMPUSER
typedef int pid_t;
#ifndef __dead
#define __dead __attribute__((__noreturn__))
#endif
#ifndef __printflike
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#endif

#include <rump/rumpuser.h>

/* NetBSD errno values: rumpuser returns these, not AmigaOS codes */
#define	RUMPUSER_EPERM		1
#define	RUMPUSER_ENOENT		2
#define	RUMPUSER_EIO		5
#define	RUMPUSER_ENOMEM		12
#define	RUMPUSER_EBUSY		16
#define	RUMPUSER_EINVAL		22
#define	RUMPUSER_EAGAIN		35
#define	RUMPUSER_EOPNOTSUPP	45
#define	RUMPUSER_ETIMEDOUT	60
#define	RUMPUSER_ENOSYS		78

/*
 * Must be called once by the task that will call rump_init(), before it
 * does so.  log is a DOS file handle (BPTR) for kernel console output.
 */
int	amiga_rump_hostinit(long log);

/* set up console logging only (log != 0); safe to call first thing */
void	amiga_rump_loginit(long log);

/* provided by rumpuser_amiga.c for host code (no C library is linked) */
void	*memset(void *, int, size_t);
void	*memcpy(void *, const void *, size_t);

/* rumpuser_component(3): host driver code entering/leaving the kernel */
struct lwp;
void	*rumpuser_component_unschedule(void);
void	rumpuser_component_schedule(void *);
void	rumpuser_component_kthread(void);
void	rumpuser_component_kthread_release(void);
struct lwp *rumpuser_component_curlwp(void);
void	rumpuser_component_switchlwp(struct lwp *);
int	rumpuser_component_errtrans(int);

/* sleep the calling host thread (must not be scheduled in the kernel) */
void	amiga_host_sleep_ms(unsigned long);
unsigned long	amiga_host_ms(void);
void	amiga_host_thread_join(void *);	/* when not on a rump CPU */
int	amiga_host_thread_done(void *);

/* debug: report CPU exceptions of the calling task on the rump console */
void	crash_install(void);

/* errno of the last failed rump_sys_*() call made by the calling thread */
int	amiga_rump_errno(void);

/* formatted output to the rump console log (same sink as kernel printf) */
void	amiga_rump_printf(const char *, ...) __printflike(1, 2);
void	amiga_rump_vprintf(const char *, va_list);
/* same, with an Amiga-style packed LONG argument array (RawDoFmt) */
void	amiga_rump_vprintf_longs(const char *, const long *);

/*
 * Set by rumpuser_exit() (RUMPUSER_PANIC for a kernel panic).  At the same
 * time amiga_rump_notifytask (default: the task that called
 * amiga_rump_hostinit()) is sent SIGBREAKF_CTRL_C.
 */
struct Task;
extern struct Task *amiga_rump_notifytask;
extern volatile int amiga_rump_exitcode;
/* nonzero: unbuffered console, RUMP_VERBOSE boot, hypercall tracing */
extern int amiga_rump_debug;
extern volatile int amiga_rump_exited;

#endif /* RUMPUSER_AMIGA_H */

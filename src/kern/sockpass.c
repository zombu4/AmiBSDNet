/*
 * Moving sockets between library bases (= NetBSD processes) for
 * bsdsocket.library's ReleaseSocket() / ObtainSocket() /
 * ReleaseCopyOfSocket().
 *
 * Called from host code running on a server thread that is bound to the
 * base's rump process; each function schedules itself.  A released
 * socket is held as a referenced file_t until it is obtained again (or
 * discarded).
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/mutex.h>
#include <sys/socketvar.h>

#include <rump/rump.h>

int	rump_amibsdnet_fd_export(int, int, void **);
int	rump_amibsdnet_fd_import(void *, int *);
void	rump_amibsdnet_fd_discard(void *);

/*
 * Take a reference to fd's file; with 'move', also remove fd from the
 * current process.
 */
int
rump_amibsdnet_fd_export(int fd, int move, void **fpp)
{
	file_t *fp;
	int error;

	rump_schedule();
	if ((fp = fd_getfile(fd)) == NULL) {
		rump_unschedule();
		return EBADF;
	}
	if (fp->f_type != DTYPE_SOCKET) {
		fd_putfile(fd);
		rump_unschedule();
		return ENOTSOCK;
	}
	mutex_enter(&fp->f_lock);
	fp->f_count++;
	mutex_exit(&fp->f_lock);
	/*
	 * fd_close() drops the fd_getfile() reference itself.  It fails
	 * (EBADF) only when another thread is closing the descriptor
	 * already (kern/kern_descrip.c:623-641): the socket is being closed
	 * by its owner, so it is not handed on and our hold goes too.
	 * Otherwise it ends in closef(), which with our hold only drops a
	 * count and returns 0 (kern_descrip.c:842-847).
	 */
	if (move) {
		if ((error = fd_close(fd)) != 0) {
			(void)closef(fp);
			rump_unschedule();
			return error;
		}
	} else
		fd_putfile(fd);
	*fpp = fp;
	rump_unschedule();
	return 0;
}

/* install a held file in the current process; the hold becomes the fd's */
int
rump_amibsdnet_fd_import(void *cookie, int *fdp)
{
	file_t *fp = cookie;
	int fd, error;

	rump_schedule();
	/* as fd_allocfile(): grow the table when it is full */
	while ((error = fd_alloc(curproc, 0, &fd)) == ENOSPC)
		fd_tryexpand(curproc);
	if (error == 0) {
		/* fd_affix() takes a reference of its own: the hold from
		   the export is dropped (2 -> 1, nothing is closed) */
		fd_affix(curproc, fp, fd);
		closef(fp);
		*fdp = fd;
	}
	rump_unschedule();
	return error;
}

/* drop a held file that nobody obtained */
void
rump_amibsdnet_fd_discard(void *cookie)
{

	rump_schedule();
	closef(cookie);
	rump_unschedule();
}

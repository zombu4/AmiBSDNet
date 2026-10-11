/*
 * bsdsocket.library: the server API, ProcessIsServer() and
 * ObtainServerSocket().
 *
 * "the doc" is downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc.
 * They concern "Programs [that] can be launched by the Internet
 * superserver" with "sockets associated with them, connected to a client
 * on the network" (the doc).  Roadshow runs that superserver inside its
 * library (the catalog, locale/bsdsocket.cd, has its 'superserver.c'
 * messages).  This stack has none: it starts no program for an incoming
 * connection, so no process is a server process and no process has a
 * server socket.  The documented results for that case are what the
 * calls return: FALSE ("TRUE if the Process in question was launched from
 * the Internet superserver, FALSE otherwise") and -1 ("if no such socket
 * is assigned or was already claimed").
 */

#include <exec/types.h>

#include "sblib.h"

BOOL
sb_ProcessIsServer(struct SocketBase *sb, struct Process *process)
{

	return FALSE;
}

LONG
sb_ObtainServerSocket(struct SocketBase *sb)
{

	return -1;
}

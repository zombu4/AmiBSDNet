/*
 * Opening SANA-II drivers by name.  They are installed in DEVS:Networks,
 * but OpenDevice() only looks in DEVS: (or finds a driver that is already
 * in memory), so "wifipi.device" works only after someone else loaded it.
 * A plain name that does not open is tried again as "Networks/<name>",
 * which OpenDevice() resolves relative to DEVS:.
 */
#ifndef AMIBSDNET_DEVOPEN_H
#define AMIBSDNET_DEVOPEN_H

#include <exec/types.h>
#include <exec/io.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

extern struct DosLibrary *DOSBase;

static inline int
amibsdnet_plain_name(const char *name)
{

	for (; *name; name++)
		if (*name == ':' || *name == '/')
			return 0;
	return 1;
}

/* "Networks/<name>" for a plain name, else the name itself */
static inline void
amibsdnet_networks_path(const char *name, char *out, int size)
{
	static const char prefix[] = "Networks/";
	int i = 0, j;

	if (amibsdnet_plain_name(name))
		for (j = 0; prefix[j] && i < size - 1; j++)
			out[i++] = prefix[j];
	for (j = 0; name[j] && i < size - 1; j++)
		out[i++] = name[j];
	out[i] = '\0';
}

/*
 * Whether a file exists, without "please insert volume" requesters (for
 * the PaulaNET: disk, which is there only with the adapter plugged in).
 * Uses dos.library: the caller must have DOSBase.
 */
static inline int
amibsdnet_exists_quiet(const char *path)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old;
	BPTR l;

	if (me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
		return 0;
	old = me->pr_WindowPtr;
	me->pr_WindowPtr = (APTR)-1;
	l = Lock((CONST_STRPTR)path, ACCESS_READ);
	me->pr_WindowPtr = old;
	if (l)
		UnLock(l);
	return l != 0;
}

/* "PaulaNET.device", also with a path in front, in any case */
static inline int
amibsdnet_is_paulanet(const char *name)
{
	static const char p[] = "paulanet.device";
	const char *s;
	int i, c;

	for (s = name; *s; s++)
		if (*s == ':' || *s == '/')
			name = s + 1;
	for (i = 0; p[i]; i++) {
		c = (UBYTE)name[i];
		if (c >= 'A' && c <= 'Z')
			c += 32;
		if (c != p[i])
			return 0;
	}
	return name[i] == '\0';
}

/*
 * OpenDevice() with the DEVS:Networks fallback; 0 on success.
 * PaulaNET.device is also looked for on the adapter's own PaulaNET: disk,
 * without a "please insert volume" requester when it is not there.
 */
static inline LONG
amibsdnet_open_sana(const char *name, ULONG unit, struct IORequest *io,
    ULONG flags)
{
	char path[96];

	/* an absolute path ("PaulaNET:PaulaNET.device"): only if the file
	   is there, since ramlib, which loads it, would ask for a missing
	   volume.  (A relative one such as "Networks/x.device" is relative
	   to DEVS: for OpenDevice(), so it cannot be checked with Lock().) */
	if (!amibsdnet_plain_name(name)) {
		const char *base = name, *p;
		int loaded, absolute = 0;

		for (p = name; *p; p++)
			if (*p == ':' || *p == '/') {
				base = p + 1;
				if (*p == ':')
					absolute = 1;
			}
		Forbid();
		loaded = FindName(&SysBase->DeviceList, (CONST_STRPTR)base) !=
		    NULL;
		Permit();
		if (absolute && !loaded && !amibsdnet_exists_quiet(name))
			return -1;
		return OpenDevice((CONST_STRPTR)name, unit, io, flags) == 0 ?
		    0 : -1;
	}
	if (OpenDevice((CONST_STRPTR)name, unit, io, flags) == 0)
		return 0;
	amibsdnet_networks_path(name, path, sizeof(path));
	if (OpenDevice((CONST_STRPTR)path, unit, io, flags) == 0)
		return 0;
	if (amibsdnet_is_paulanet(name) &&
	    amibsdnet_exists_quiet("PaulaNET:PaulaNET.device"))
		return OpenDevice((CONST_STRPTR)"PaulaNET:PaulaNET.device", unit,
		    io, flags) == 0 ? 0 : -1;
	return -1;
}

/*
 * The name a program such as WirelessManager should use: the plain name
 * if the driver is in memory already, else its DEVS:Networks path.
 */
static inline void
amibsdnet_device_arg(struct ExecBase *sysbase, const char *name, char *out,
    int size)
{
	int loaded;

	(void)sysbase;
	Forbid();
	loaded = FindName(&SysBase->DeviceList, (CONST_STRPTR)name) != NULL;
	Permit();
	if (loaded || !amibsdnet_plain_name(name)) {
		int i;

		for (i = 0; name[i] && i < size - 1; i++)
			out[i] = name[i];
		out[i] = '\0';
	} else
		amibsdnet_networks_path(name, out, size);
}

#endif /* AMIBSDNET_DEVOPEN_H */

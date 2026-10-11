/*
 * Opening SANA-II drivers by name.  They are installed in DEVS:Networks.
 * OpenDevice() takes a device that is in memory or on disk, a plain name
 * being one of the system's DEVS: directory, and "A full path name for the
 * device name is legitimate.  For example "test:devs/fred.device""
 * (downloads/sources/NDK3.2/Autodocs/exec.doc, OpenDevice FUNCTION).  A
 * path relative to DEVS: is not documented, so a plain name that does not
 * open is tried again as the full path "DEVS:Networks/<name>", and a
 * relative path such as "Networks/x.device" is opened as "DEVS:<path>".
 *
 * DOS requesters (a missing volume) are turned off around every
 * OpenDevice() made here, as the autodoc says it can be done: "You must
 * call this function from a DOS Process if you want to turn off DOS
 * requesters" (exec.doc, OpenDevice NOTES), with pr_WindowPtr -1
 * (dos.doc ErrorReport).
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

static inline int
amibsdnet_has_colon(const char *name)
{

	for (; *name; name++)
		if (*name == ':')
			return 1;
	return 0;
}

/* "DEVS:Networks/<name>" for a plain name, "DEVS:<name>" for a relative
   path, else the name itself */
static inline void
amibsdnet_networks_path(const char *name, char *out, int size)
{
	const char *prefix = amibsdnet_plain_name(name) ? "DEVS:Networks/" :
	    !amibsdnet_has_colon(name) ? "DEVS:" : "";
	int i = 0, j;

	for (j = 0; prefix[j] && i < size - 1; j++)
		out[i++] = prefix[j];
	for (j = 0; name[j] && i < size - 1; j++)
		out[i++] = name[j];
	out[i] = '\0';
}

/* the driver's name without its path */
static inline const char *
amibsdnet_base_name(const char *name)
{
	const char *base = name, *p;

	for (p = name; *p; p++)
		if (*p == ':' || *p == '/')
			base = p + 1;
	return base;
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
	int i, c;

	name = amibsdnet_base_name(name);
	for (i = 0; p[i]; i++) {
		c = (UBYTE)name[i];
		if (c >= 'A' && c <= 'Z')
			c += 32;
		if (c != p[i])
			return 0;
	}
	return name[i] == '\0';
}

/* is a device of this name in memory (exec lists are case sensitive:
   exec.doc OpenDevice BUGS) */
static inline int
amibsdnet_loaded(const char *name)
{
	int loaded;

	Forbid();
	loaded = FindName(&SysBase->DeviceList, (CONST_STRPTR)name) != NULL;
	Permit();
	return loaded;
}

/* OpenDevice() without DOS requesters (from a process); 0 on success */
static inline LONG
amibsdnet_opendevice(const char *name, ULONG unit, struct IORequest *io,
    ULONG flags)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	int proc = me->pr_Task.tc_Node.ln_Type == NT_PROCESS;
	APTR old = NULL;
	LONG r;

	if (proc) {
		old = me->pr_WindowPtr;
		me->pr_WindowPtr = (APTR)-1;
	}
	r = OpenDevice((CONST_STRPTR)name, unit, io, flags);
	if (proc)
		me->pr_WindowPtr = old;
	return r;
}

/*
 * OpenDevice() with the DEVS:Networks fallback; 0 on success.
 * PaulaNET.device is also looked for on the adapter's own PaulaNET: disk,
 * only when that file is there.
 */
static inline LONG
amibsdnet_open_sana(const char *name, ULONG unit, struct IORequest *io,
    ULONG flags)
{
	char path[96];

	if (!amibsdnet_plain_name(name)) {
		/* already in memory: by its name; else the full path */
		if (amibsdnet_loaded(amibsdnet_base_name(name)))
			return amibsdnet_opendevice(amibsdnet_base_name(name),
			    unit, io, flags) == 0 ? 0 : -1;
		amibsdnet_networks_path(name, path, sizeof(path));
		if (!amibsdnet_exists_quiet(path))
			return -1;
		return amibsdnet_opendevice(path, unit, io, flags) == 0 ? 0 : -1;
	}
	if (amibsdnet_opendevice(name, unit, io, flags) == 0)
		return 0;
	/* in memory now (it was, or DEVS: had it): that device refused the
	   open; another path would only meet a device of the same name */
	if (amibsdnet_loaded(name))
		return -1;
	amibsdnet_networks_path(name, path, sizeof(path));
	if (amibsdnet_exists_quiet(path) &&
	    amibsdnet_opendevice(path, unit, io, flags) == 0)
		return 0;
	if (amibsdnet_is_paulanet(name) &&
	    amibsdnet_exists_quiet("PaulaNET:PaulaNET.device"))
		return amibsdnet_opendevice("PaulaNET:PaulaNET.device", unit,
		    io, flags) == 0 ? 0 : -1;
	return -1;
}

/*
 * The name a program such as WirelessManager should use: the plain name
 * if the driver is in memory already, else its full DEVS:Networks path.
 */
static inline void
amibsdnet_device_arg(struct ExecBase *sysbase, const char *name, char *out,
    int size)
{

	(void)sysbase;
	if (amibsdnet_loaded(name) || amibsdnet_has_colon(name)) {
		int i;

		for (i = 0; name[i] && i < size - 1; i++)
			out[i] = name[i];
		out[i] = '\0';
	} else
		amibsdnet_networks_path(name, out, size);
}

#endif /* AMIBSDNET_DEVOPEN_H */

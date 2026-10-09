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
#include <proto/exec.h>

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

/* OpenDevice() with the DEVS:Networks fallback; 0 on success */
static inline LONG
amibsdnet_open_sana(const char *name, ULONG unit, struct IORequest *io,
    ULONG flags)
{
	char path[96];

	if (OpenDevice((CONST_STRPTR)name, unit, io, flags) == 0)
		return 0;
	if (!amibsdnet_plain_name(name))
		return -1;
	amibsdnet_networks_path(name, path, sizeof(path));
	return OpenDevice((CONST_STRPTR)path, unit, io, flags) == 0 ? 0 : -1;
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

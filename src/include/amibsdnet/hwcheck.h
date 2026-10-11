/*
 * Is what a Raspberry Pi driver needs there, before the driver is opened?
 * Sources (pinned copies in downloads/sources/):
 *
 *  - genet.device 3.10 (github.com/rondoval/emu68-genet-driver):
 *    README.md: "Broadcom GENET v5 Ethernet controller found on the
 *    Raspberry PI 4B and CM4", "3.x requires gic400.library".  The driver
 *    reads the device tree aliases "ethernet0" and "gpio"
 *    (genet.device/src/devtree_parse.c:31-33) and opens gic400.library
 *    (device.c:146).  The model check here is the README's requirement;
 *    the driver itself does not read the model.  Its version (2.2 or
 *    newer: README "2.2: Fix an issue where the driver will attempt to
 *    process an S2_ONLINE request while unconfigured and crash") is
 *    read here, before the open, from its file (hw_genet_version()).
 *    A genet.device already in memory is not asked: the driver sets only
 *    lib_Revision itself (device.c:173), and exec.doc InitResident and
 *    MakeLibrary do not say that lib_Version or lib_IdString are taken
 *    from the Resident (rt_Version, rt_IdString: device.c:76-85), so its
 *    file is checked as well.
 *  - wifipi.device (github.com/michalsc/WiFiPi.device, src/init.c:38-82):
 *    the root node's "compatible" against its model table, then a
 *    firmware set (.bin, .clm_blob or none, .txt) from DEVS:Firmware
 *    chosen by the Wi-Fi chip.  This check cannot read the chip, so a
 *    model passes when one of its sets is complete.  (wifipi.c:1167
 *    ignores a failed firmware load and :1228 then reads from the NULL
 *    firmware base.)  Not detectable here: a CM4 without Wi-Fi.
 *  - devicetree.resource (github.com/michalsc/devicetree.resource):
 *    interface from sfd/devicetree_lib.sfd (bias 6); Emu68 has it in its
 *    ROM (Emu68 src/boards/emu68rom.c:35), so without it this is not
 *    Emu68.
 *
 * Other drivers are not checked (1: go ahead).
 */
#ifndef AMIBSDNET_HWCHECK_H
#define AMIBSDNET_HWCHECK_H

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

/* devicetree.resource, bias 6: OpenKey -6, CloseKey -12, FindProperty
   -24, GetPropLen -36, GetPropValue -48 */
static inline APTR
hw_dt_openkey(APTR base, CONST_STRPTR name)
{
	register APTR res __asm("d0");
	register APTR a0 __asm("a0") = (APTR)name;
	register APTR a6 __asm("a6") = base;

	__asm volatile ("jsr -6(%%a6)"
	    : "=r"(res), "+r"(a0)
	    : "r"(a6)
	    : "d1", "a1", "fp0", "fp1", "cc", "memory");
	return res;
}

static inline void
hw_dt_closekey(APTR base, APTR key)
{
	register APTR a0 __asm("a0") = key;
	register APTR a6 __asm("a6") = base;

	__asm volatile ("jsr -12(%%a6)"
	    : "+r"(a0)
	    : "r"(a6)
	    : "d0", "d1", "a1", "fp0", "fp1", "cc", "memory");
}

static inline APTR
hw_dt_findproperty(APTR base, APTR key, CONST_STRPTR name)
{
	register APTR res __asm("d0");
	register APTR a0 __asm("a0") = key;
	register APTR a1 __asm("a1") = (APTR)name;
	register APTR a6 __asm("a6") = base;

	__asm volatile ("jsr -24(%%a6)"
	    : "=r"(res), "+r"(a0), "+r"(a1)
	    : "r"(a6)
	    : "d1", "fp0", "fp1", "cc", "memory");
	return res;
}

static inline ULONG
hw_dt_getproplen(APTR base, APTR prop)
{
	register ULONG res __asm("d0");
	register APTR a0 __asm("a0") = prop;
	register APTR a6 __asm("a6") = base;

	__asm volatile ("jsr -36(%%a6)"
	    : "=r"(res), "+r"(a0)
	    : "r"(a6)
	    : "d1", "a1", "fp0", "fp1", "cc", "memory");
	return res;
}

static inline CONST_APTR
hw_dt_getpropvalue(APTR base, APTR prop)
{
	register CONST_APTR res __asm("d0");
	register APTR a0 __asm("a0") = prop;
	register APTR a6 __asm("a6") = base;

	__asm volatile ("jsr -48(%%a6)"
	    : "=r"(res), "+r"(a0)
	    : "r"(a6)
	    : "d1", "a1", "fp0", "fp1", "cc", "memory");
	return res;
}

static inline int
hw_eq_nocase(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
		char y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;

		if (x != y)
			return 0;
	}
	return *a == *b;
}

/* the driver's name without a path */
static inline const char *
hw_basename(const char *name)
{
	const char *b = name, *p;

	for (p = name; *p; p++)
		if (*p == ':' || *p == '/')
			b = p + 1;
	return b;
}

static inline void
hw_copy(char *d, const char *s, int n)
{

	while (--n > 0 && *s)
		*d++ = *s++;
	*d = '\0';
}

/*
 * The Pi model ("compatible" of the device tree's root node, e.g.
 * "raspberrypi,4-model-b") into out; 0 if this is not Emu68 or it does
 * not say.  (The property is a list of strings: the first is the model.)
 */
static inline int
hw_pi_model(char *out, int size)
{
	APTR dt, key, prop;
	const char *v;

	out[0] = '\0';
	if ((dt = OpenResource((CONST_STRPTR)"devicetree.resource")) == NULL)
		return 0;
	if ((key = hw_dt_openkey(dt, (CONST_STRPTR)"/")) == NULL)
		return 0;
	if ((prop = hw_dt_findproperty(dt, key, (CONST_STRPTR)"compatible"))
	    != NULL && hw_dt_getproplen(dt, prop) > 0 &&
	    (v = (const char *)hw_dt_getpropvalue(dt, prop)) != NULL)
		hw_copy(out, v, size);
	hw_dt_closekey(dt, key);
	return out[0] != '\0';
}

/* does the device tree node path have the property? */
static inline int
hw_dt_has(const char *path, const char *property)
{
	APTR dt, key;
	int has = 0;

	if ((dt = OpenResource((CONST_STRPTR)"devicetree.resource")) == NULL)
		return 0;
	if ((key = hw_dt_openkey(dt, (CONST_STRPTR)path)) == NULL)
		return 0;
	has = hw_dt_findproperty(dt, key, (CONST_STRPTR)property) != NULL;
	hw_dt_closekey(dt, key);
	return has;
}

static inline int
hw_file_exists(const char *path)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR old = me->pr_WindowPtr;
	BPTR l;

	me->pr_WindowPtr = (APTR)-1;
	l = Lock((CONST_STRPTR)path, ACCESS_READ);
	me->pr_WindowPtr = old;
	if (l)
		UnLock(l);
	return l != 0;
}

/*
 * The version in genet.device's file: its id string is compiled in
 * (genet device.c:66 deviceIdString), either "$VER: <project> x.y (date)"
 * (CMakeLists.txt:5 VERSTRING, genet.device/CMakeLists.txt:50) or
 * "genet.device x.y" (device.c:32, the default).  1 and *ver, *rev if
 * one of them is found.
 */
static inline int
hw_genet_version(const char *path, LONG *ver, LONG *rev)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR old = me->pr_WindowPtr;
	static const char *const marks[] = { "$VER: ", "genet.device " };
	UBYTE buf[1024];
	BPTR fh;
	LONG n, i, keep = 0;
	int found = 0, m;

	me->pr_WindowPtr = (APTR)-1;
	fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
	me->pr_WindowPtr = old;
	if (fh == 0)
		return 0;
	while (!found && (n = Read(fh, buf + keep, sizeof(buf) - keep)) > 0) {
		n += keep;
		for (i = 0; i < n && !found; i++)
			for (m = 0; m < 2 && !found; m++) {
				const char *k = marks[m];
				LONG j = i, v = 0, r = 0;
				int l;

				for (l = 0; k[l] && j < n && buf[j] == (UBYTE)k[l];
				    l++, j++)
					;
				if (k[l])
					continue;
				/* "$VER: " is followed by the project name */
				if (m == 0) {
					while (j < n && buf[j] != ' ' && buf[j])
						j++;
					if (j < n && buf[j] == ' ')
						j++;
				}
				if (j >= n || buf[j] < '0' || buf[j] > '9')
					continue;
				while (j < n && buf[j] >= '0' && buf[j] <= '9')
					v = v * 10 + buf[j++] - '0';
				if (j >= n || buf[j] != '.')
					continue;
				for (j++; j < n && buf[j] >= '0' && buf[j] <= '9';
				    j++)
					r = r * 10 + buf[j] - '0';
				*ver = v;
				*rev = r;
				found = 1;
			}
		/* (the last 64 bytes again: a string split by the read) */
		keep = n > 64 ? 64 : n;
		for (i = 0; i < keep; i++)
			buf[i] = buf[n - keep + i];
	}
	Close(fh);
	return found;
}

/*
 * 1: the hardware for this driver is there (or it is not one of the
 * checked drivers); 0: it is not, and why says why.
 */
static inline int
hw_check(const char *device, unsigned long unit, char *why, int size)
{
	const char *name = hw_basename(device);
	char model[64];
	/* genet's alias for the unit: "ethernet0" with the unit's digit
	   (downloads/sources/genet-3.10/genet.device/src/devtree_parse.c:
	   28-29, alias[8] = (char)('0' + unit->unitNumber)) */
	char alias[12] = "ethernet0";
	static const char nomsg[] = "the device tree has no gpio alias or ";
	char msg[sizeof(nomsg) + sizeof(alias)];
	int k, l;

	why[0] = '\0';
	if (hw_eq_nocase(name, "genet.device")) {
		int gic;

		if (!hw_pi_model(model, sizeof(model))) {
			hw_copy(why, "no devicetree.resource: this is not Emu68",
			    size);
			return 0;
		}
		/* (README.md: "Raspberry PI 4B and CM4") */
		if (!hw_eq_nocase(model, "raspberrypi,4-model-b") &&
		    !hw_eq_nocase(model, "raspberrypi,4-compute-module")) {
			hw_copy(why, "it is for the Raspberry Pi 4B and CM4",
			    size);
			return 0;
		}
		/* (devtree_parse.c:31-33: both aliases or it fails) */
		alias[8] = (char)('0' + unit);
		if (!hw_dt_has("/aliases", alias) ||
		    !hw_dt_has("/aliases", "gpio")) {
			for (l = 0; nomsg[l]; l++)
				msg[l] = nomsg[l];
			for (k = 0; alias[k]; k++)
				msg[l++] = alias[k];
			msg[l] = '\0';
			hw_copy(why, msg, size);
			return 0;
		}
		/*
		 * gic400.library (device.c:146 opens it): in memory, or
		 * resident in Emu68's ROM (Emu68 src/boards/emu68rom.c:42;
		 * its Resident is named "gic400.library" and flagged
		 * RTF_AUTOINIT | RTF_COLDSTART: emu68-gic400-library
		 * CMakeLists.txt:2,59, src/gic400_main.c:15,19-29), or in
		 * LIBS:
		 */
		Forbid();
		gic = FindName(&SysBase->LibList, (CONST_STRPTR)
		    "gic400.library") != NULL;
		Permit();
		if (!gic)
			gic = FindResident((CONST_STRPTR)"gic400.library") !=
			    NULL;
		if (!gic && !hw_file_exists("LIBS:gic400.library")) {
			hw_copy(why, "gic400.library is not there (not in "
			    "memory, not in Emu68's ROM, not in LIBS:)", size);
			return 0;
		}
		/* 2.2 or newer (README.md "2.2: Fix an issue where the
		   driver will attempt to process an S2_ONLINE request while
		   unconfigured and crash"); a version that cannot be read
		   is not taken as new enough */
		{
			LONG v = 0, r = 0;

			if (!hw_genet_version("DEVS:Networks/genet.device", &v,
			    &r) && !hw_genet_version("DEVS:genet.device", &v,
			    &r)) {
				hw_copy(why, "its version cannot be read from "
				    "DEVS:Networks/genet.device (2.2 or newer is "
				    "needed)", size);
				return 0;
			}
			if (v < 2 || (v == 2 && r < 2)) {
				hw_copy(why, "it is older than 2.2 (older ones "
				    "crash on S2_ONLINE when unconfigured: its "
				    "README)", size);
				return 0;
			}
		}
		return 1;
	}
	if (hw_eq_nocase(name, "wifipi.device")) {
		/* WiFiPi.device src/init.c:38-82: per model the firmware
		   sets (.bin, .clm_blob or none, .txt); the chip decides */
		static const struct {
			const char *model, *bin, *clm, *txt;
		} fw[] = {
			{ "raspberrypi,model-zero-2-w", "brcmfmac43436s-sdio.bin",
			    NULL, "brcmfmac43436s-sdio.txt" },
			{ "raspberrypi,model-zero-2-w", "brcmfmac43436-sdio.bin",
			    "brcmfmac43436-sdio.clm_blob",
			    "brcmfmac43436-sdio.txt" },
			{ "raspberrypi,3-model-b", "brcmfmac43436s-sdio.bin",
			    NULL, "brcmfmac43436s-sdio.txt" },
			{ "raspberrypi,3-model-a-plus", "cyfmac43455-sdio.bin",
			    "cyfmac43455-sdio.clm_blob", "brcmfmac43455-sdio.txt" },
			{ "raspberrypi,3-model-b-plus", "cyfmac43455-sdio.bin",
			    "cyfmac43455-sdio.clm_blob", "brcmfmac43455-sdio.txt" },
			{ "raspberrypi,4-model-b", "cyfmac43455-sdio.bin",
			    "cyfmac43455-sdio.clm_blob", "brcmfmac43455-sdio.txt" },
			{ "raspberrypi,4-compute-module", "cyfmac43455-sdio.bin",
			    "cyfmac43455-sdio.clm_blob", "brcmfmac43455-sdio.txt" },
			{ "raspberrypi,4-compute-module", "brcmfmac43456-sdio.bin",
			    "brcmfmac43456-sdio.clm_blob",
			    "brcmfmac43456-sdio.txt" },
		};
		char p[96];
		int i, known = 0;

		if (!hw_pi_model(model, sizeof(model))) {
			hw_copy(why, "no devicetree.resource: this is not Emu68",
			    size);
			return 0;
		}
		for (i = 0; i < (int)(sizeof(fw) / sizeof(fw[0])); i++) {
			if (!hw_eq_nocase(model, fw[i].model))
				continue;
			known = 1;
			hw_copy(p, "DEVS:Firmware/", sizeof(p));
			hw_copy(p + 14, fw[i].bin, sizeof(p) - 14);
			if (!hw_file_exists(p))
				continue;
			if (fw[i].clm) {
				hw_copy(p + 14, fw[i].clm, sizeof(p) - 14);
				if (!hw_file_exists(p))
					continue;
			}
			hw_copy(p + 14, fw[i].txt, sizeof(p) - 14);
			if (hw_file_exists(p))
				return 1;	/* a complete set */
		}
		hw_copy(why, known ? "its firmware files for this Raspberry "
		    "Pi are not complete in DEVS:Firmware (.bin, .clm_blob, "
		    ".txt)" : "this Raspberry Pi model is not in its model "
		    "table", size);
		return 0;
	}
	return 1;
}

#endif /* AMIBSDNET_HWCHECK_H */

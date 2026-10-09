/*
 * WirelessManager helpers, shared by the stack (autostart) and the status
 * tool (Wi-Fi window).
 *
 * Wireless SANA-II drivers (e.g. Emu68's wifipi.device) only scan and
 * associate; the WPA handshake is done by WirelessManager, a
 * wpa_supplicant port that reads ENV:Sys/Wireless.prefs:
 *
 *   network={
 *           ssid="MyNetwork"
 *           psk="passphrase"
 *           key_mgmt=WPA-PSK         (key_mgmt=NONE for open networks)
 *   }
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <amibsdnet/wm.h>
#include <amibsdnet/devopen.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

#define	WM_COMMAND	"C:WirelessManager"
#define	WM_LOG		"T:WirelessManager.log"
#define	PREFS_ENV	"ENV:Sys/Wireless.prefs"
#define	PREFS_ENVARC	"ENVARC:Sys/Wireless.prefs"

static int
contains_nocase(const char *hay, int haylen, const char *needle)
{
	int i, j;

	for (i = 0; i < haylen; i++) {
		for (j = 0; needle[j] && i + j < haylen; j++) {
			int a = (UBYTE)hay[i + j], b = (UBYTE)needle[j];

			if (a >= 'A' && a <= 'Z') a += 32;
			if (b >= 'A' && b <= 'Z') b += 32;
			if (a != b)
				break;
		}
		if (!needle[j])
			return 1;
	}
	return 0;
}

static int
slen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

/* is this task WirelessManager?  On Kickstart 3.x a Shell command keeps the
   CLI's task name, so also look at the CLI's command name */
static int
is_wm(struct Task *t)
{
	const char *name = t->tc_Node.ln_Name;

	if (name && contains_nocase(name, slen(name), "WirelessManager"))
		return 1;
	if (t->tc_Node.ln_Type == NT_PROCESS) {
		struct Process *p = (struct Process *)t;
		struct CommandLineInterface *cli = BADDR(p->pr_CLI);

		if (cli && cli->cli_CommandName) {
			const UBYTE *b = BADDR(cli->cli_CommandName);

			if (b && b[0] && contains_nocase((const char *)b + 1, b[0],
			    "WirelessManager"))
				return 1;
		}
	}
	return 0;
}

/* call under Forbid() */
static struct Task *
find_wm(void)
{
	struct Node *n;

	if (is_wm(SysBase->ThisTask))
		return SysBase->ThisTask;
	for (n = SysBase->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
		if (is_wm((struct Task *)n))
			return (struct Task *)n;
	for (n = SysBase->TaskWait.lh_Head; n->ln_Succ; n = n->ln_Succ)
		if (is_wm((struct Task *)n))
			return (struct Task *)n;
	return NULL;
}

int
wm_running(void)
{
	int r;

	Forbid();
	r = find_wm() != NULL;
	Permit();
	return r;
}

int
wm_installed(void)
{
	BPTR l = Lock((CONST_STRPTR)WM_COMMAND, ACCESS_READ);

	if (l) {
		UnLock(l);
		return 1;
	}
	return 0;
}

int
wm_stop(void)
{
	struct Task *t;
	int i;

	Forbid();
	if ((t = find_wm()) != NULL)
		Signal(t, SIGBREAKF_CTRL_C);
	Permit();
	for (i = 0; i < 40 && wm_running(); i++)
		Delay(10);
	return wm_running() ? -1 : 0;
}

/*
 * WirelessManager's messages go to T:WirelessManager.log (VERBOSE if the
 * variable AmiBSDNet/Debug is set); the file stays readable while it runs.
 */
int
wm_start(const char *device, unsigned long unit)
{
	char cmd[200], devarg[96], dbg[4], *p = cmd;
	const char *s;
	BPTR in, out;

	if (!wm_installed())
		return -1;
	/* a driver not yet in memory is opened from DEVS:Networks */
	amibsdnet_device_arg(SysBase, device, devarg, sizeof(devarg));
	for (s = WM_COMMAND " \""; *s; )
		*p++ = *s++;
	for (s = devarg; *s && p < cmd + 140; )
		*p++ = *s++;
	*p++ = '"';
	if (GetVar((CONST_STRPTR)"AmiBSDNet/Debug", (STRPTR)dbg, sizeof(dbg),
	    0) >= 0)
		for (s = " VERBOSE"; *s; )
			*p++ = *s++;
	if (unit) {
		char tmp[12];
		int i = 0;

		for (s = " UNIT "; *s; )
			*p++ = *s++;
		do {
			tmp[i++] = '0' + unit % 10;
			unit /= 10;
		} while (unit);
		while (i)
			*p++ = tmp[--i];
	}
	*p = '\0';
	in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
	if ((out = Open((CONST_STRPTR)WM_LOG, MODE_NEWFILE)) != 0) {
		Close(out);
		out = Open((CONST_STRPTR)WM_LOG, MODE_READWRITE);
	}
	if (out == 0)
		out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
	if (SystemTags((CONST_STRPTR)cmd, SYS_Asynch, TRUE, SYS_Input, in,
	    SYS_Output, out, NP_StackSize, 65536, TAG_DONE) != 0) {
		if (in) Close(in);
		if (out) Close(out);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * Wireless.prefs
 */

static char *
read_file(const char *name, LONG *lenp)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
	char *buf = NULL;
	LONG len = 0, n;

	if (fh) {
		if ((buf = AllocVec(16384, MEMF_ANY | MEMF_CLEAR)) != NULL) {
			while (len < 16383 &&
			    (n = Read(fh, buf + len, 16383 - len)) > 0)
				len += n;
		}
		Close(fh);
	}
	*lenp = len;
	return buf;
}

/* does a network={...} block (from p to the next '}') name this SSID? */
static int
block_has_ssid(const char *p, const char *end, const char *ssid)
{
	int sl = slen(ssid);

	for (; p + 6 + sl < end; p++)
		if (p[0] == 's' && p[1] == 's' && p[2] == 'i' && p[3] == 'd' &&
		    p[4] == '=' && p[5] == '"') {
			int i;

			for (i = 0; i < sl && p[6 + i] == ssid[i]; i++)
				;
			if (i == sl && p[6 + sl] == '"')
				return 1;
		}
	return 0;
}

static int
write_prefs(const char *name, const char *old, LONG oldlen, const char *ssid,
    const char *psk)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_NEWFILE);
	const char *p = old, *end = old + oldlen;

	if (fh == 0)
		return -1;
	/* the chosen network first, so it is preferred */
	FPuts(fh, (CONST_STRPTR)"network={\n\tssid=\"");
	FPuts(fh, (CONST_STRPTR)ssid);
	FPuts(fh, (CONST_STRPTR)"\"\n");
	if (psk && psk[0]) {
		FPuts(fh, (CONST_STRPTR)"\tpsk=\"");
		FPuts(fh, (CONST_STRPTR)psk);
		FPuts(fh, (CONST_STRPTR)"\"\n\tkey_mgmt=WPA-PSK\n");
	} else
		FPuts(fh, (CONST_STRPTR)"\tkey_mgmt=NONE\n");
	FPuts(fh, (CONST_STRPTR)"}\n\n");

	/* keep everything else, minus any older block for the same SSID */
	while (old && p < end) {
		const char *b = p, *e;
		int i;

		for (; b + 8 < end; b++)
			if (b[0] == 'n' && b[1] == 'e' && b[2] == 't' &&
			    b[3] == 'w' && b[4] == 'o' && b[5] == 'r' &&
			    b[6] == 'k' && b[7] == '=')
				break;
		if (b + 8 >= end) {
			FWrite(fh, (APTR)p, end - p, 1);
			break;
		}
		FWrite(fh, (APTR)p, b - p, 1);
		for (e = b; e < end && *e != '}'; e++)
			;
		if (e < end)
			e++;
		for (i = 0; e < end && (*e == '\n' || *e == '\r') && i < 2; i++)
			e++;
		if (!block_has_ssid(b, e, ssid))
			FWrite(fh, (APTR)b, e - b, 1);
		p = e;
	}
	Close(fh);
	return 0;
}

int
wm_set_network(const char *ssid, const char *psk)
{
	LONG len;
	char *old = read_file(PREFS_ENVARC, &len);
	BPTR l;
	int rv;

	if (old == NULL)
		old = read_file(PREFS_ENV, &len);
	/* make sure the Sys drawers exist */
	if ((l = CreateDir((CONST_STRPTR)"ENVARC:Sys")) != 0)
		UnLock(l);
	if ((l = CreateDir((CONST_STRPTR)"ENV:Sys")) != 0)
		UnLock(l);
	rv = write_prefs(PREFS_ENVARC, old, len, ssid, psk);
	if (write_prefs(PREFS_ENV, old, len, ssid, psk) != 0)
		rv = -1;
	if (old)
		FreeVec(old);
	return rv;
}

int
wm_has_network(const char *ssid)
{
	LONG len;
	char *buf = read_file(PREFS_ENV, &len);
	int r = 0;

	if (buf) {
		r = block_has_ssid(buf, buf + len, ssid);
		FreeVec(buf);
	}
	return r;
}

/* the preferred (first) network in Wireless.prefs; 0 if there is one */
static int
get_value(const char *b, const char *end, const char *key, char *out, int n)
{
	int kl = slen(key), i;

	for (; b + kl + 2 < end && *b != '}'; b++)
		if ((b[-1] == '\t' || b[-1] == ' ' || b[-1] == '\n' ||
		    b[-1] == '{') && b[kl] == '=' && b[kl + 1] == '"') {
			for (i = 0; i < kl && b[i] == key[i]; i++)
				;
			if (i < kl)
				continue;
			b += kl + 2;
			for (i = 0; b < end && *b != '"' && i < n - 1; i++)
				out[i] = *b++;
			out[i] = '\0';
			return 1;
		}
	return 0;
}

int
wm_get_network(char *ssid, int ssidlen, char *psk, int psklen)
{
	LONG len;
	char *buf = read_file(PREFS_ENV, &len), *p, *end;
	int r = -1;

	if (buf == NULL)
		buf = read_file(PREFS_ENVARC, &len);
	ssid[0] = psk[0] = '\0';
	if (buf) {
		end = buf + len;
		for (p = buf; p + 9 < end; p++)
			if (p[0] == 'n' && p[1] == 'e' && p[2] == 't' &&
			    p[3] == 'w' && p[4] == 'o' && p[5] == 'r' &&
			    p[6] == 'k' && p[7] == '=' && p[8] == '{') {
				if (get_value(p + 9, end, "ssid", ssid, ssidlen)) {
					get_value(p + 9, end, "psk", psk, psklen);
					r = 0;
				}
				break;
			}
		FreeVec(buf);
	}
	return r;
}

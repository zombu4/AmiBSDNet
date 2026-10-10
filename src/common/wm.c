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

		/* (a Shell keeps the name of its last command: only while
		   that command runs, cli_Module is set) */
		if (cli && cli->cli_Module && cli->cli_CommandName) {
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
	/* (leaving the network can take a while) */
	for (i = 0; i < 75 && wm_running(); i++)
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
	/* the argument line; kept static, a new process may read it late */
	static char cmd[200];
	char devarg[96], dbg[4], *p = cmd;
	const char *s;
	BPTR in, out, seg;

	if (!wm_installed())
		return -1;
	/* never two: both would talk to the same driver */
	if (wm_running())
		return -2;
	/* a driver not yet in memory is opened from DEVS:Networks */
	amibsdnet_device_arg(SysBase, device, devarg, sizeof(devarg));
	*p++ = '"';
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
	*p++ = '\n';
	*p = '\0';
	in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
	if ((out = Open((CONST_STRPTR)WM_LOG, MODE_NEWFILE)) != 0) {
		Close(out);
		out = Open((CONST_STRPTR)WM_LOG, MODE_READWRITE);
	}
	if (out == 0)
		out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
	/*
	 * Its own process with a 64 KB stack: started through a Shell
	 * (System()) a command gets the Shell's default stack, often 4 KB,
	 * far too little for wpa_supplicant.  The task name is how
	 * wm_running() finds it.
	 */
	if ((seg = LoadSeg((CONST_STRPTR)WM_COMMAND)) == 0 ||
	    CreateNewProcTags(NP_Seglist, seg, NP_FreeSeglist, TRUE,
	    NP_Name, (ULONG)"WirelessManager", NP_StackSize, 65536,
	    NP_Cli, TRUE, NP_Arguments, (ULONG)cmd,
	    NP_Input, in, NP_Output, out, NP_CloseInput, TRUE,
	    NP_CloseOutput, TRUE, TAG_DONE) == NULL) {
		if (seg)
			UnLoadSeg(seg);
		if (in) Close(in);
		if (out) Close(out);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * Wireless.prefs
 */

/* the whole file (NUL-terminated); NULL if there is none, or with *errp
   set if it is there but could not be read (it must not be rewritten
   from a part of it then) */
static char *
read_file_err(const char *name, LONG *lenp, int *errp)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
	char *buf = NULL;
	LONG len = 0, size, n;

	*errp = 0;
	*lenp = 0;
	if (fh == 0)
		return NULL;
	if (Seek(fh, 0, OFFSET_END) < 0)
		size = -1;
	else
		size = Seek(fh, 0, OFFSET_BEGINNING);
	if (size < 0 || size > 1024 * 1024 ||
	    (buf = AllocVec(size + 1, MEMF_ANY | MEMF_CLEAR)) == NULL) {
		Close(fh);
		*errp = 1;
		return NULL;
	}
	while (len < size && (n = Read(fh, buf + len, size - len)) > 0)
		len += n;
	Close(fh);
	if (len != size) {
		FreeVec(buf);
		*errp = 1;
		return NULL;
	}
	*lenp = len;
	return buf;
}

static char *
read_file(const char *name, LONG *lenp)
{
	int err;

	return read_file_err(name, lenp, &err);
}

/* the end of a network={...} block starting at b: after the line that
   starts with '}' (as wpa_supplicant writes them; a '}' inside a quoted
   SSID does not end it) */
static const char *
block_end(const char *b, const char *end)
{
	const char *p = b;

	while (p < end) {
		while (p < end && *p != '\n')
			p++;
		if (p < end)
			p++;
		while (p < end && (*p == ' ' || *p == '\t'))
			p++;
		if (p < end && *p == '}')
			return p + 1;
	}
	return end;
}

static int get_value(const char *, const char *, const char *, char *, int);

/* does the network={...} block at p ("network=") name this SSID?  The
   SSID is read as get_value() reads it, so "a" and "a"b" differ */
static int
block_has_ssid(const char *p, const char *end, const char *ssid)
{
	char v[128];
	int i;

	if (end - p < 9 || !get_value(p + 8, end, "ssid", v, sizeof(v)) ||
	    slen(v) == sizeof(v) - 1)	/* (cut: not a real SSID) */
		return 0;
	for (i = 0; v[i] && v[i] == ssid[i]; i++)
		;
	return v[i] == '\0' && ssid[i] == '\0';
}

/* only blanks between the start of p's line and p (so a "# network="
   comment, or text in a value, is not taken for a keyword) */
static int
at_line_start(const char *start, const char *p)
{

	while (p > start && (p[-1] == ' ' || p[-1] == '\t'))
		p--;
	return p == start || p[-1] == '\n' || p[-1] == '\r';
}

/* a key inside a block: only blanks before it on its line ("#\tssid="
   is a comment).  There is always a "network=" earlier in the buffer, so
   this never looks before it. */
static int
key_at_line_start(const char *p)
{

	while (p[-1] == ' ' || p[-1] == '\t')
		p--;
	return p[-1] == '\n' || p[-1] == '\r';
}

/* the next "network=" at the start of a line, from p; NULL if none */
static const char *
next_block(const char *start, const char *p, const char *end)
{

	for (; p + 8 < end; p++)
		if (p[0] == 'n' && p[1] == 'e' && p[2] == 't' && p[3] == 'w' &&
		    p[4] == 'o' && p[5] == 'r' && p[6] == 'k' && p[7] == '=' &&
		    at_line_start(start, p))
			return p;
	return NULL;
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
		const char *b = next_block(old, p, end), *e;
		int i;

		if (b == NULL) {
			FWrite(fh, (APTR)p, end - p, 1);
			break;
		}
		FWrite(fh, (APTR)p, b - p, 1);
		e = block_end(b, end);
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
	int err;
	char *old = read_file_err(PREFS_ENVARC, &len, &err);
	BPTR l;
	int rv;

	if (old == NULL && !err)
		old = read_file_err(PREFS_ENV, &len, &err);
	if (err)
		return -1;	/* never rewrite it from a part */
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
		const char *p = buf, *end = buf + len;

		while (!r && (p = next_block(buf, p, end)) != NULL) {
			r = block_has_ssid(p, end, ssid);
			p = block_end(p, end);
		}
		FreeVec(buf);
	}
	return r;
}

/* the preferred (first) network in Wireless.prefs; 0 if there is one */
static int
get_value(const char *b, const char *end, const char *key, char *out, int n)
{
	int kl = slen(key), i;
	const char *q, *eol;

	end = block_end(b, end);
	for (; b + kl + 2 < end; b++)
		if (b[kl] == '=' && b[kl + 1] == '"' && key_at_line_start(b)) {
			for (i = 0; i < kl && b[i] == key[i]; i++)
				;
			if (i < kl)
				continue;
			b += kl + 2;
			/* up to the last '"' of the line, as wpa_supplicant
			   reads it (a passphrase may contain '"') */
			for (eol = b; eol < end && *eol != '\n' && *eol != '\r';
			    eol++)
				;
			for (q = eol; q > b && q[-1] != '"'; q--)
				;
			if (q > b)
				q--;		/* at the closing quote */
			else
				q = eol;
			for (i = 0; b < q && i < n - 1; i++)
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
			    p[6] == 'k' && p[7] == '=' && p[8] == '{' &&
			    at_line_start(buf, p)) {
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

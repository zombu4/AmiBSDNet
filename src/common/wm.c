/*
 * WirelessManager helpers, shared by the stack (autostart) and the status
 * tool (Wi-Fi window).
 *
 * WirelessManager is a port of wpa_supplicant 0.7.3 (downloads/sources/
 * AROS/WirelessManager: README, wpa_supplicant/ChangeLog); it reads its
 * networks from ENV:Sys/Wireless.prefs (wpa_supplicant/main_amiga.c,
 * config_file_name) in wpa_supplicant's file syntax (.config:
 * CONFIG_BACKEND=file, so wpa_supplicant/config_file.c):
 *
 *   network={
 *           ssid="MyNetwork"         (or the SSID in hex, not quoted)
 *           psk="passphrase"         (or 64 hex digits, not quoted)
 *           key_mgmt=WPA-PSK         (key_mgmt=NONE for open networks)
 *   }
 *
 * How it reads that file (config_file.c wpa_config_get_line(),
 * wpa_config_read_network(); config.c wpa_config_parse_string(),
 * wpa_config_parse_psk()): blanks at the start of a line are skipped, '#'
 * starts a comment (after the last '"' of the line if it has one),
 * trailing blanks go; a block starts with a line "network={" and ends
 * with a line "}"; inside, a line is <key>=<value>.  A quoted value runs
 * to the last '"', which must end the line; an unquoted SSID is hex.
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
#include <amibsdnet/logs.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

#define	WM_COMMAND	"C:WirelessManager"
#define	PREFS_ENV	"ENV:Sys/Wireless.prefs"
#define	PREFS_ENVARC	"ENVARC:Sys/Wireless.prefs"

int	wm_passphrase_ok(const char *psk);
int	amibsdnet_replace_file(const char *name, const char *data, long len);
void	wm_signal_stop(void);

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

/* is this task WirelessManager?  By its task name, or, for a Shell
   process, by the command it runs: cli_CommandName is the "Name of
   current command", cli_Module the "SegList of currently loaded command"
   (NDK dos/dosextens.h), so the name counts only while cli_Module is
   set */
static int
is_wm(struct Task *t)
{
	const char *name = t->tc_Node.ln_Name;

	if (name && contains_nocase(name, slen(name), "WirelessManager"))
		return 1;
	if (t->tc_Node.ln_Type == NT_PROCESS) {
		struct Process *p = (struct Process *)t;
		struct CommandLineInterface *cli = BADDR(p->pr_CLI);

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

/* asks WirelessManager to end (Ctrl-C), without waiting */
void
wm_signal_stop(void)
{
	struct Task *t;

	Forbid();
	if ((t = find_wm()) != NULL)
		Signal(t, SIGBREAKF_CTRL_C);
	Permit();
}

int
wm_stop(void)
{
	int i;

	wm_signal_stop();
	/* (leaving the network can take a while) */
	for (i = 0; i < 75 && wm_running(); i++)
		Delay(10);
	return wm_running() ? -1 : 0;
}

/*
 * WirelessManager's messages go to AMIBSDNET_WMLOG, on disk (T: if that
 * cannot be written; amibsdnet/logs.h) (VERBOSE if the
 * variable AmiBSDNet/Debug is set); the file stays readable while it runs.
 */
int
wm_start(const char *device, unsigned long unit)
{
	/* the argument line (CreateNewProc() copies it: dos.doc
	   NP_Arguments "The arguments are copied before they are passed
	   in") */
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
	/* "if you call CreateNewProc() with both NP_Arguments, you must not
	   specify an NP_Input of NULL" (dos.doc CreateNewProc): no start
	   without it */
	if ((in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE)) == 0)
		return -1;
	out = amibsdnet_log_create(AMIBSDNET_WMLOG, AMIBSDNET_WMLOG_T, NULL);
	if (out == 0)
		out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
	if (out == 0) {
		Close(in);
		return -1;
	}
	/*
	 * Its own process with a 64 KB stack (a command run through a Shell
	 * gets that Shell's stack size).  The task name is how wm_running()
	 * finds it.
	 */
	if ((seg = LoadSeg((CONST_STRPTR)WM_COMMAND)) == 0 ||
	    CreateNewProcTags(NP_Seglist, seg, NP_FreeSeglist, TRUE,
	    NP_Name, (ULONG)"WirelessManager", NP_StackSize, 65536,
	    NP_Cli, TRUE, NP_Arguments, (ULONG)cmd,
	    NP_Input, in, NP_Output, out, NP_CloseInput, TRUE,
	    NP_CloseOutput, TRUE, TAG_DONE) == NULL) {
		if (seg)
			UnLoadSeg(seg);
		Close(in);
		Close(out);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * files
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
	if (fh == 0) {
		/* (not there, or its drawer is not: no file; anything else
		   is an error) */
		LONG e = IoErr();

		if (e != ERROR_OBJECT_NOT_FOUND && e != ERROR_DIR_NOT_FOUND)
			*errp = 1;
		return NULL;
	}
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

static void
cat3(char *d, const char *a, const char *b)
{

	while (*a)
		*d++ = *a++;
	while (*b)
		*d++ = *b++;
	*d = '\0';
}

/* does the file hold exactly these bytes? */
static int
same_data(const char *name, const char *data, LONG len)
{
	LONG n, i;
	char *buf = read_file(name, &n);
	int same;

	if (buf == NULL)
		return 0;
	same = n == len;
	for (i = 0; same && i < len; i++)
		if (buf[i] != data[i])
			same = 0;
	FreeVec(buf);
	return same;
}

/* the file's text becomes these bytes, read back to be sure; 1 if done */
static int
write_whole(const char *name, const char *data, LONG len)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_NEWFILE);
	int ok;

	if (fh == 0)
		return 0;
	ok = len == 0 || Write(fh, (APTR)data, len) == len;
	if (!Close(fh))
		ok = 0;
	return ok && (len == 0 || same_data(name, data, len));
}

int
amibsdnet_replace_file(const char *name, const char *data, long len)
{
	char tmp[256], old[256], comment[80];
	struct FileInfoBlock *fib;
	LONG prot = -1;
	BPTR fh, l;
	int ok, hadold = 0, kept = 0, i;

	/* (".amibsdnet-new" and ".amibsdnet-old": 14 characters) */
	if (slen(name) + 14 + 1 > (int)sizeof(tmp))
		return -1;
	cat3(tmp, name, ".amibsdnet-new");
	cat3(old, name, ".amibsdnet-old");
	comment[0] = '\0';
	/*
	 * A <name>.amibsdnet-old is left by a call that failed after the
	 * original had gone there (below): then it is still the original,
	 * and <name> is missing or holds a text that could not be written
	 * in full.  Or by a call that succeeded but could not delete it (at
	 * the end): then it is the text before that call's.  Either way it
	 * stays as the copy kept and the attributes are its.  With such a
	 * kept copy, a failure of this call before it deletes <name>
	 * (below) leaves <name> as it is; after that, the kept text is what
	 * is put back into <name> (or stays in <name>.amibsdnet-old), so in
	 * the second case the text the earlier call wrote is lost.
	 */
	if ((l = Lock((CONST_STRPTR)old, ACCESS_READ)) != 0)
		kept = 1;
	else
		l = Lock((CONST_STRPTR)name, ACCESS_READ);
	hadold = kept || l != 0;
	/* the original: its protection bits and comment go to the new file;
	   one that is write-protected is not replaced */
	if (l != 0) {
		if ((fib = AllocDosObject(DOS_FIB, NULL)) == NULL) {
			UnLock(l);
			return -1;
		}
		if (Examine(l, fib)) {
			prot = fib->fib_Protection;
			for (i = 0; i < 79 && fib->fib_Comment[i]; i++)
				comment[i] = fib->fib_Comment[i];
			comment[i] = '\0';
		}
		FreeDosObject(DOS_FIB, fib);
		UnLock(l);
		if (prot == -1)
			return -1;
		/* "the low order four bits, i.e. 'rwed', are low-active"
		   (downloads/sources/NDK3.2/Autodocs/dos.doc:5426,
		   SetProtection): a set FIBF_WRITE means no writing */
		if (prot & FIBF_WRITE)
			return -1;
	}
	if ((fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE)) == 0)
		return -1;
	ok = len == 0 || Write(fh, (APTR)data, len) == len;
	if (!Close(fh))
		ok = 0;
	/* and read back */
	if (ok && len > 0)
		ok = same_data(tmp, data, len);
	if (!ok) {
		DeleteFile((CONST_STRPTR)tmp);
		return -1;
	}
	/*
	 * The original text is never lost: it is in <name> or in
	 * <name>.amibsdnet-old all the time.  The original is renamed out of
	 * the way ("If the file or directory 'newName' exists, Rename()
	 * fails", dos.doc Rename), so for a moment <name> is missing, and it
	 * is put back if the new file cannot take its place.  With a kept
	 * <name>.amibsdnet-old (above) <name> is deleted instead (if it is
	 * there).
	 */
	if (kept) {
		if ((l = Lock((CONST_STRPTR)name, ACCESS_READ)) != 0) {
			UnLock(l);
			if (!DeleteFile((CONST_STRPTR)name)) {
				DeleteFile((CONST_STRPTR)tmp);
				return -1;
			}
		}
	} else if (hadold && !Rename((CONST_STRPTR)name, (CONST_STRPTR)old)) {
		DeleteFile((CONST_STRPTR)tmp);
		return -1;
	}
	if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)name)) {
		/*
		 * AmigaOS 3.2's RAM-Disk mirrors ENVARC: into ENV: and "pulls
		 * the target file into the RAM-Disk transparently" when it is
		 * looked up (NDK3.2/ReleaseNotes/ram-handler-RelNotes:39-48):
		 * with the original out of the way the name is there again,
		 * and Rename() onto it fails (tests/replprobe-wb32.txt).  The
		 * new text then goes into that file itself; if that fails
		 * too, the original text goes back into it.
		 */
		if (hadold && (l = Lock((CONST_STRPTR)name, ACCESS_READ)) != 0) {
			char *orig;
			LONG olen;

			UnLock(l);
			DeleteFile((CONST_STRPTR)tmp);
			if (write_whole(name, data, len)) {
				DeleteFile((CONST_STRPTR)old);
				goto done;
			}
			/* (if this fails as well, the original stays in
			   <name>.amibsdnet-old, which the next call keeps as
			   the original, above) */
			if ((orig = read_file(old, &olen)) != NULL) {
				if (write_whole(name, orig, olen)) {
					DeleteFile((CONST_STRPTR)old);
					if (prot != -1)
						SetProtection((CONST_STRPTR)name,
						    prot);
					if (comment[0])
						SetComment((CONST_STRPTR)name,
						    (CONST_STRPTR)comment);
				}
				FreeVec(orig);
			}
			return -1;
		}
		if (hadold)
			Rename((CONST_STRPTR)old, (CONST_STRPTR)name);
		DeleteFile((CONST_STRPTR)tmp);
		return -1;
	}
	/* (also a kept one.  If it cannot be deleted it stays, and the next
	   call takes it for the original: that call still writes its new
	   text, and only if that call fails does this older text come
	   back) */
	DeleteFile((CONST_STRPTR)old);
done:
	if (prot != -1)
		SetProtection((CONST_STRPTR)name, prot);
	if (comment[0])
		SetComment((CONST_STRPTR)name, (CONST_STRPTR)comment);
	return 0;
}

/* ------------------------------------------------------------------------
 * Wireless.prefs
 */

static int
hexval(int c)
{

	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* is p..e a line WirelessManager reads as this text?  (blanks before;
   after it blanks, CR, or a '#' comment: the line has no '"') */
static int
line_is(const char *p, const char *e, const char *text)
{
	int i;

	while (p < e && (*p == ' ' || *p == '\t' || *p == '\r'))
		p++;
	for (i = 0; text[i]; i++, p++)
		if (p >= e || *p != text[i])
			return 0;
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\r'))
		p++;
	return p == e || *p == '#';
}

static const char *
eol(const char *p, const char *end)
{

	while (p < end && *p != '\n')
		p++;
	return p;
}

/* the next "network={" line from p (p at a line start); NULL if none */
static const char *
next_block(const char *p, const char *end)
{
	const char *e;

	for (; p < end; p = e + 1) {
		e = eol(p, end);
		if (line_is(p, e, "network={"))
			return p;
	}
	return NULL;
}

/* the end of the block whose "network={" line starts at b: the start of
   the line after its "}" line (or end) */
static const char *
block_end(const char *b, const char *end)
{
	const char *p = eol(b, end), *e;

	for (p = p < end ? p + 1 : p; p < end; p = e + 1) {
		e = eol(p, end);
		if (line_is(p, e, "}"))
			return e < end ? e + 1 : e;
	}
	return end;
}

/*
 * The value of a key in the block at b: 1 for a quoted one (the text
 * between the quotes), 2 for one without quotes (the text itself), 0 if
 * the block has no such line or the value is not readable so.
 */
static int
get_value(const char *b, const char *end, const char *key, char *out, int n)
{
	int kl = slen(key), i;
	const char *p, *e, *v, *ve, *q;

	end = block_end(b, end);
	for (p = eol(b, end); p < end; p = e + 1) {
		if (*p == '\n')
			p++;
		e = eol(p, end);
		while (p < e && (*p == ' ' || *p == '\t' || *p == '\r'))
			p++;
		if (e - p <= kl)
			continue;
		for (i = 0; i < kl && p[i] == key[i]; i++)
			;
		if (i < kl || p[kl] != '=')
			continue;
		v = p + kl + 1;
		/* the comment: from the first '#' after the last '"' (or
		   after the start if there is no '"') */
		for (q = e; q > v && q[-1] != '"'; q--)
			;
		for (ve = q; ve < e && *ve != '#'; ve++)
			;
		while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' ||
		    ve[-1] == '\r'))
			ve--;
		if (ve > v && *v == '"') {
			/* the last '"' must end the value */
			if (ve - v < 2 || ve[-1] != '"')
				return 0;
			v++;
			ve--;
			if (ve - v > n - 1)
				return 0;
			for (i = 0; v < ve; i++)
				out[i] = *v++;
			out[i] = '\0';
			return 1;
		}
		if (ve - v > n - 1)
			return 0;
		for (i = 0; v < ve; i++)
			out[i] = *v++;
		out[i] = '\0';
		return 2;
	}
	return 0;
}

/* the SSID of the block at b, as WirelessManager reads it (hex when not
   quoted); 0 if none */
static int
block_ssid(const char *b, const char *end, char *ssid, int n)
{
	char v[72];
	int r = get_value(b, end, "ssid", v, sizeof(v)), i, hi, lo;

	if (r == 1) {
		for (i = 0; v[i] && i < n - 1; i++)
			ssid[i] = v[i];
		ssid[i] = '\0';
		return v[i] == '\0';
	}
	if (r != 2 || (slen(v) & 1) || slen(v) / 2 > n - 1)
		return 0;
	for (i = 0; v[2 * i]; i++) {
		if ((hi = hexval((UBYTE)v[2 * i])) < 0 ||
		    (lo = hexval((UBYTE)v[2 * i + 1])) < 0)
			return 0;
		ssid[i] = hi * 16 + lo;
	}
	ssid[i] = '\0';
	return 1;
}

static int
block_has_ssid(const char *b, const char *end, const char *ssid)
{
	char v[40];
	int i;

	if (!block_ssid(b, end, v, sizeof(v)))
		return 0;
	for (i = 0; v[i] && v[i] == ssid[i]; i++)
		;
	return v[i] == '\0' && ssid[i] == '\0';
}

/* 64 hex digits: a PSK itself, "entered either as 64 hex-digits, i.e., 32
   bytes or as an ASCII passphrase" (downloads/sources/hostap/
   wpa_supplicant.conf, psk) */
static int
raw_psk(const char *psk)
{
	int i;

	for (i = 0; psk[i]; i++)
		if (hexval((UBYTE)psk[i]) < 0)
			return 0;
	return i == 64;
}

/* a passphrase this project writes for WirelessManager: 8 to 63
   characters, the length WirelessManager accepts (downloads/sources/AROS/
   WirelessManager/wpa_supplicant/config.c, wpa_config_parse_psk: "len < 8
   || len > 63" is an error), each printable ASCII (0x20 to 0x7e: this
   project's own rule, so that every one can be typed and written on one
   line); or 64 hex digits, the PSK itself (raw_psk()) */
int
wm_passphrase_ok(const char *psk)
{
	int i;

	if (raw_psk(psk))
		return 1;
	for (i = 0; psk[i]; i++)
		if ((UBYTE)psk[i] < 0x20 || (UBYTE)psk[i] > 0x7e)
			return 0;
	return i >= 8 && i <= 63;
}

static char *
put(char *d, const char *s)
{

	while (*s)
		*d++ = *s++;
	return d;
}

/* does the line p..e set one of these keys ("key=")? */
static int
line_sets(const char *p, const char *e, const char *const *keys)
{
	int i, k;

	while (p < e && (*p == ' ' || *p == '\t'))
		p++;
	for (k = 0; keys[k]; k++) {
		for (i = 0; keys[k][i] && p + i < e && p[i] == keys[k][i]; i++)
			;
		if (!keys[k][i] && p + i < e && p[i] == '=')
			return 1;
	}
	return 0;
}

static const char *const ssid_key[] = { "ssid", NULL };
/* the security settings of a network (hostap/wpa_supplicant.conf:
   proto, key_mgmt, auth_alg, pairwise, group, psk, sae_password,
   wep_key0..3, wep_tx_keyidx) */
static const char *const key_keys[] = {
	"ssid", "proto", "key_mgmt", "auth_alg", "pairwise", "group", "psk",
	"sae_password", "wep_key0", "wep_key1", "wep_key2", "wep_key3",
	"wep_tx_keyidx", NULL
};

/* the lines inside the block at b (without "network={", "}" and the
   lines setting one of keys) to d */
static char *
put_block_lines(char *d, const char *b, const char *end,
    const char *const *keys)
{
	const char *be = block_end(b, end), *p, *e;

	for (p = eol(b, be); p < be; p = e) {
		if (*p == '\n')
			p++;
		e = eol(p, be);
		if (line_is(p, e, "}"))
			break;
		if (p < e && !line_sets(p, e, keys)) {
			while (p < e)
				*d++ = *p++;
			*d++ = '\n';
		}
	}
	return d;
}

/* the new text: the chosen network first, so it is preferred, then
   everything else minus any older block for the same SSID.  The other
   settings of the first older block for it (scan_ssid, priority...) are
   kept; with psk NULL its security settings too */
static char *
new_prefs(const char *old, LONG oldlen, const char *ssid, const char *psk,
    LONG *lenp)
{
	const char *p = old, *end = old + oldlen, *b, *e, *ob = NULL;
	char *buf, *d;
	int i, plain = 1;

	if ((buf = AllocVec(oldlen + 256, MEMF_ANY)) == NULL)
		return NULL;
	for (b = old ? next_block(p, end) : NULL; b && !ob;
	    b = next_block(block_end(b, end), end))
		if (block_has_ssid(b, block_end(b, end), ssid))
			ob = b;
	d = put(buf, "network={\n\tssid=");
	/* quoted only when every byte is printable ASCII and none is a
	   '"'; else hex ("a hex string (two characters per octet of
	   SSID)", hostap/wpa_supplicant.conf, ssid; read by config.c
	   wpa_config_parse_string()) */
	for (i = 0; ssid[i]; i++)
		if ((UBYTE)ssid[i] < 0x20 || (UBYTE)ssid[i] > 0x7e ||
		    ssid[i] == '"')
			plain = 0;
	if (plain) {
		*d++ = '"';
		d = put(d, ssid);
		*d++ = '"';
	} else
		for (i = 0; ssid[i]; i++) {
			*d++ = "0123456789abcdef"[(UBYTE)ssid[i] >> 4];
			*d++ = "0123456789abcdef"[(UBYTE)ssid[i] & 15];
		}
	*d++ = '\n';
	if (psk == NULL && ob)
		d = put_block_lines(d, ob, end, ssid_key);
	else if (psk && psk[0]) {
		/* a passphrase quoted: config.c wpa_config_parse_psk() takes
		   it up to the last '"', so it may contain '"' itself */
		if (raw_psk(psk)) {
			d = put(d, "\tpsk=");
			d = put(d, psk);
		} else {
			d = put(d, "\tpsk=\"");
			d = put(d, psk);
			*d++ = '"';
		}
		d = put(d, "\n\tkey_mgmt=WPA-PSK\n");
	} else
		d = put(d, "\tkey_mgmt=NONE\n");
	if (ob && psk != NULL)
		d = put_block_lines(d, ob, end, key_keys);
	d = put(d, "}\n\n");

	while (old && p < end) {
		if ((b = next_block(p, end)) == NULL) {
			for (; p < end; p++)
				*d++ = *p;
			break;
		}
		for (; p < b; p++)
			*d++ = *p;
		e = block_end(b, end);
		/* (one empty line after a block goes with it) */
		if (e < end && *e == '\n')
			e++;
		else if (e + 1 < end && e[0] == '\r' && e[1] == '\n')
			e += 2;
		if (!block_has_ssid(b, e, ssid))
			for (; b < e; b++)
				*d++ = *b;
		p = e;
	}
	*lenp = d - buf;
	return buf;
}

/*
 * ENV:Sys/Wireless.prefs as it is becomes the saved one, ENVARC:'s (the
 * Settings window's Save with the network unchanged there: after a Use,
 * the network in use is in ENV: only).  The text is copied, not built
 * again, so every line of every block stays (a WEP or 802.1X block too).
 * 0 also when there is no ENV: file (nothing to save); -1 if it cannot be
 * read in full or ENVARC: cannot be written.
 */
int
wm_save_env(void)
{
	LONG len;
	int err, rv;
	char *buf = read_file_err(PREFS_ENV, &len, &err);
	BPTR l;

	if (err)
		return -1;
	if (buf == NULL)
		return 0;
	if ((l = CreateDir((CONST_STRPTR)"ENVARC:Sys")) != 0)
		UnLock(l);
	rv = amibsdnet_replace_file(PREFS_ENVARC, buf, len);
	FreeVec(buf);
	return rv;
}

static int
write_prefs(const char *name, const char *old, LONG oldlen, const char *ssid,
    const char *psk)
{
	LONG len;
	char *buf = new_prefs(old, oldlen, ssid, psk, &len);
	int rv;

	if (buf == NULL)
		return -1;
	rv = amibsdnet_replace_file(name, buf, len);
	FreeVec(buf);
	return rv;
}

int
wm_set_network(const char *ssid, const char *psk, int save)
{
	LONG len, envlen;
	int err, rv;
	char *old, *envold;
	BPTR l;

	/* (an SSID is at most 32 bytes; a passphrase as wm_passphrase_ok()
	   says) */
	if (!ssid[0] || slen(ssid) > 32 || (psk && psk[0] &&
	    !wm_passphrase_ok(psk)))
		return -1;
	/* each file from its own text: what was added to one only stays */
	old = read_file_err(PREFS_ENVARC, &len, &err);
	if (err)
		return -1;	/* never rewrite it from a part */
	envold = read_file_err(PREFS_ENV, &envlen, &err);
	if (err) {
		if (old)
			FreeVec(old);
		return -1;
	}
	/* make sure the Sys drawers exist */
	if (save && (l = CreateDir((CONST_STRPTR)"ENVARC:Sys")) != 0)
		UnLock(l);
	if ((l = CreateDir((CONST_STRPTR)"ENV:Sys")) != 0)
		UnLock(l);
	rv = save ? write_prefs(PREFS_ENVARC, old, len, ssid, psk) : 0;
	if (write_prefs(PREFS_ENV, envold ? envold : old, envold ? envlen :
	    len, ssid, psk) != 0)
		rv = -1;
	if (old)
		FreeVec(old);
	if (envold)
		FreeVec(envold);
	return rv;
}

/* the preferred (first) network in Wireless.prefs: 0 if there is one */
int
wm_get_network(char *ssid, int ssidlen, char *psk, int psklen)
{
	LONG len;
	char *buf = read_file(PREFS_ENV, &len);
	const char *p, *end;
	int r = -1;

	if (buf == NULL)
		buf = read_file(PREFS_ENVARC, &len);
	ssid[0] = psk[0] = '\0';
	if (buf) {
		end = buf + len;
		if ((p = next_block(buf, end)) != NULL &&
		    block_ssid(p, end, ssid, ssidlen)) {
			if (!get_value(p, end, "psk", psk, psklen))
				psk[0] = '\0';
			r = 0;
		}
		FreeVec(buf);
	}
	return r;
}

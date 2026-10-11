/*
 * Checking third-party drivers before they are used or installed.
 *
 * PaulaNET.device (https://github.com/RobSmithDev/PaulaNET) comes on the
 * adapter's own floppy image, and copies of it can be damaged.  It cannot
 * be shipped with AmiBSDNet (its repository has no licence), so the
 * official builds are recognised instead, by size and CRC32 (taken from
 * the disk images in the PaulaNET 1.0 and 1.1 firmware sources).
 *
 * Verifying is off by default: a newer PaulaNET release would not be in
 * the table.  With "paulanet verify on" only a known build, or one whose
 * CRC the user accepted ("paulanet crc ..."), is used.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <amibsdnet/drvcheck.h>
#include <amibsdnet/devopen.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

static const struct {
	ULONG	size;
	ULONG	crc;
	const char *version;
} known_paulanet[] = {
	{ 13324, 0xf801b5a9UL, "1.0 (2026-05-11)" },
	{ 13872, 0x24282c8eUL, "1.1 (2026-06-13)" },
};

static int
lower(int c)
{

	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int
eq_nocase(const char *a, const char *b)
{

	while (*a && lower((UBYTE)*a) == lower((UBYTE)*b))
		a++, b++;
	return *a == *b;
}

int
drv_parse_crc(const char *s, ULONG *v)
{
	ULONG n = 0;
	int i, c;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	for (i = 0; s[i]; i++) {
		c = lower((UBYTE)s[i]);
		if (i >= 8)
			return 0;
		if (c >= '0' && c <= '9')
			n = n << 4 | (c - '0');
		else if (c >= 'a' && c <= 'f')
			n = n << 4 | (c - 'a' + 10);
		else
			return 0;
	}
	if (i == 0)
		return 0;
	*v = n;
	return 1;
}

void
drv_fmt_crc(char *out, ULONG crc)
{
	int i;

	for (i = 0; i < 8; i++)
		out[i] = "0123456789abcdef"[(crc >> (28 - 4 * i)) & 15];
	out[8] = '\0';
}

int
drv_parse_line(struct drvprefs *p, char **tok, int n)
{
	ULONG v;

	if (n < 3 || !eq_nocase(tok[0], "paulanet"))
		return 0;
	if (eq_nocase(tok[1], "verify"))
		p->verify = eq_nocase(tok[2], "on");
	else if (eq_nocase(tok[1], "crc") && p->ncrc < DRV_MAXCRC &&
	    drv_parse_crc(tok[2], &v))
		p->crc[p->ncrc++] = v;
	return 1;
}

void
drv_read_prefs(struct drvprefs *p)
{
	char line[200], *tok[4];
	BPTR fh;
	int n;
	char *s;

	p->verify = 0;
	p->ncrc = 0;
	if ((fh = Open((CONST_STRPTR)"ENV:AmiBSDNet/AmiBSDNet.conf",
	    MODE_OLDFILE)) == 0 &&
	    (fh = Open((CONST_STRPTR)"ENVARC:AmiBSDNet/AmiBSDNet.conf",
	    MODE_OLDFILE)) == 0)
		return;
	/* (the size less one for dos V36/V37, which copy one byte more:
	   dos.doc:2146-2150, FGets BUGS) */
	while (FGets(fh, (STRPTR)line, sizeof(line) - 1)) {
		for (n = 0, s = line; n < 4;) {
			while (*s == ' ' || *s == '\t')
				s++;
			if (!*s || *s == '#' || *s == ';' || *s == '\n' ||
			    *s == '\r')
				break;
			tok[n++] = s;
			while (*s && *s != ' ' && *s != '\t' && *s != '\n' &&
			    *s != '\r')
				s++;
			if (!*s)
				break;
			*s++ = '\0';
		}
		drv_parse_line(p, tok, n);
	}
	Close(fh);
}

int
drv_paulanet_path(char *out, int size)
{
	static const char *const where[] = {
		"DEVS:" PAULANET_NAME,
		"DEVS:Networks/" PAULANET_NAME,
		"PaulaNET:" PAULANET_NAME,
	};
	unsigned i;
	int j;

	for (i = 0; i < sizeof(where) / sizeof(where[0]); i++)
		if (amibsdnet_exists_quiet(where[i])) {
			for (j = 0; where[i][j] && j < size - 1; j++)
				out[j] = where[i][j];
			out[j] = '\0';
			return 1;
		}
	return 0;
}

static ULONG
crc32(ULONG crc, const UBYTE *p, LONG n)
{
	int k;

	crc = ~crc;
	while (n-- > 0) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320UL & -(crc & 1));
	}
	return ~crc;
}

int	drv_file_sum(const char *path, unsigned long *size, unsigned long *crc);

/* size and CRC-32 of a file, read in pieces (any size); 0 when it could
   all be read (NetCtrl CHECKSUM, the uninstaller's FILE lines) */
int
drv_file_sum(const char *path, unsigned long *size, unsigned long *crc)
{
	BPTR fh;
	UBYTE *buf;
	LONG n;
	ULONG c = 0, total = 0;

	if ((buf = AllocVec(4096, MEMF_ANY)) == NULL)
		return -1;
	if ((fh = Open((CONST_STRPTR)path, MODE_OLDFILE)) == 0) {
		FreeVec(buf);
		return -1;
	}
	while ((n = Read(fh, buf, 4096)) > 0) {
		c = crc32(c, buf, n);
		total += n;
	}
	Close(fh);
	FreeVec(buf);
	/* (Read(): 0 at the end of the file, -1 for an error, downloads/
	   sources/NDK3.2/Autodocs/dos.doc Read) */
	if (n < 0)
		return -1;
	*size = total;
	*crc = c;
	return 0;
}

static int check(const char *, const struct drvprefs *, const char **,
    ULONG *);

/* without requesters: a read error on the PaulaNET: disk must not stop
   the stack's start (or the installer) at a Retry/Cancel requester */
int
drv_check_paulanet(const char *path, const struct drvprefs *p,
    const char **version, ULONG *crcp)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR old;
	int r;

	*version = NULL;
	*crcp = 0;
	if (me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
		return DRV_MISSING;
	old = me->pr_WindowPtr;
	me->pr_WindowPtr = (APTR)-1;
	r = check(path, p, version, crcp);
	me->pr_WindowPtr = old;
	return r;
}

static int
check(const char *path, const struct drvprefs *p, const char **version,
    ULONG *crcp)
{
	BPTR fh, seg;
	UBYTE *buf;
	LONG size, n, got = 0;
	ULONG crc;
	unsigned i;
	int k;

	*version = NULL;
	*crcp = 0;
	if (!amibsdnet_exists_quiet(path) ||
	    (fh = Open((CONST_STRPTR)path, MODE_OLDFILE)) == 0)
		return DRV_MISSING;
	Seek(fh, 0, OFFSET_END);
	size = Seek(fh, 0, OFFSET_BEGINNING);
	if (size <= 0 || size > 256 * 1024 ||
	    (buf = AllocVec(size, MEMF_ANY)) == NULL) {
		Close(fh);
		return DRV_DAMAGED;
	}
	while (got < size && (n = Read(fh, buf + got, size - got)) > 0)
		got += n;
	Close(fh);
	if (got != size) {
		FreeVec(buf);
		return DRV_DAMAGED;		/* a read error */
	}
	*crcp = crc = crc32(0, buf, size);
	FreeVec(buf);
	for (i = 0; i < sizeof(known_paulanet) / sizeof(known_paulanet[0]);
	    i++)
		if ((ULONG)size == known_paulanet[i].size &&
		    crc == known_paulanet[i].crc) {
			*version = known_paulanet[i].version;
			return DRV_OK;
		}
	if (p)
		for (k = 0; k < p->ncrc; k++)
			if (crc == p->crc[k])
				return DRV_ACCEPTED;
	/* not a build we know: a newer one, if it loads as a program */
	if ((seg = LoadSeg((CONST_STRPTR)path)) == 0)
		return DRV_DAMAGED;
	UnLoadSeg(seg);
	return DRV_UNKNOWN;
}

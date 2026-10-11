/*
 * The access point side of fakewifi.device (src/test/fakewifi.c): a
 * WPA2-PSK (CCMP) authenticator for the 4-way handshake over EAPOL-Key
 * frames.  With wifipi.device the handshake is run by WirelessManager,
 * not by the chip: wifipi sets "sup_wpa" 0 (downloads/sources/wifipi/
 * src/unit.c:1534), and WirelessManager's driver says "we run the
 * handshake here and hand the driver the keys with S2_SETKEY"
 * (downloads/sources/AROS/WirelessManager/src/drivers/driver_sana2.c:
 * 498-501).  Test only.
 *
 * The parts and where they are specified:
 *   SHA-1                FIPS 180-4 (sections 4.1.1, 4.2.1, 5.1.1, 6.1.2)
 *   HMAC                 RFC 2104
 *   PSK -> PMK           PBKDF2 (RFC 8018 5.2) with HMAC-SHA1, 4096
 *                        iterations, 32 bytes, salt = SSID (IEEE
 *                        802.11-2016 J.4)
 *   PTK                  PRF-384 (IEEE 802.11-2016 12.7.1.2): HMAC-SHA1
 *                        of A || 0 || B || i; A "Pairwise key
 *                        expansion", B the two addresses and the two
 *                        nonces, smaller first (12.7.1.3)
 *   EAPOL-Key frame      IEEE 802.11-2016 12.7.2; descriptor version 2:
 *                        MIC HMAC-SHA1-128 over the frame with the MIC
 *                        field zero, key data AES key wrap (RFC 3394)
 *   AES-128              FIPS 197
 * src/test/fakeaptest.c checks the functions against test vectors.
 */
#include <exec/types.h>

#include "fakeap.h"

/* ------------------------------------------------------------------------
 * SHA-1 (FIPS 180-4)
 */

struct sha1 {
	ULONG h[5];
	ULONG len;		/* bytes so far (the frames here are small) */
	UBYTE buf[64];
	int n;
};

#define	ROL(x, n)	(((x) << (n)) | ((x) >> (32 - (n))))

static void
sha1_block(struct sha1 *c, const UBYTE *p)
{
	ULONG w[80], a, b, d, e, f, k, t, cc;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (ULONG)p[4 * i] << 24 | (ULONG)p[4 * i + 1] << 16 |
		    (ULONG)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = c->h[0];
	b = c->h[1];
	cc = c->h[2];
	d = c->h[3];
	e = c->h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20) {
			f = (b & cc) | (~b & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ cc ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & cc) | (b & d) | (cc & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ cc ^ d;
			k = 0xca62c1d6;
		}
		t = ROL(a, 5) + f + e + k + w[i];
		e = d;
		d = cc;
		cc = ROL(b, 30);
		b = a;
		a = t;
	}
	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += cc;
	c->h[3] += d;
	c->h[4] += e;
}

static void
sha1_init(struct sha1 *c)
{

	c->h[0] = 0x67452301;
	c->h[1] = 0xefcdab89;
	c->h[2] = 0x98badcfe;
	c->h[3] = 0x10325476;
	c->h[4] = 0xc3d2e1f0;
	c->len = 0;
	c->n = 0;
}

static void
sha1_update(struct sha1 *c, const UBYTE *p, ULONG n)
{

	c->len += n;
	while (n--) {
		c->buf[c->n++] = *p++;
		if (c->n == 64) {
			sha1_block(c, c->buf);
			c->n = 0;
		}
	}
}

static void
sha1_final(struct sha1 *c, UBYTE out[20])
{
	ULONG bits = c->len * 8;
	UBYTE z = 0x80;
	int i;

	sha1_update(c, &z, 1);
	z = 0;
	while (c->n != 56)
		sha1_update(c, &z, 1);
	for (i = 0; i < 4; i++) {
		z = 0;
		sha1_update(c, &z, 1);
	}
	for (i = 3; i >= 0; i--) {
		z = bits >> (8 * i);
		sha1_update(c, &z, 1);
	}
	for (i = 0; i < 5; i++) {
		out[4 * i] = c->h[i] >> 24;
		out[4 * i + 1] = c->h[i] >> 16;
		out[4 * i + 2] = c->h[i] >> 8;
		out[4 * i + 3] = c->h[i];
	}
}

void
fakeap_sha1(const UBYTE *p, ULONG n, UBYTE out[20])
{
	struct sha1 c;

	sha1_init(&c);
	sha1_update(&c, p, n);
	sha1_final(&c, out);
}

/* ------------------------------------------------------------------------
 * HMAC-SHA1 (RFC 2104), over up to three pieces of data
 */

void
fakeap_hmac_sha1(const UBYTE *key, ULONG klen, const UBYTE *a, ULONG alen,
    const UBYTE *b, ULONG blen, const UBYTE *cp, ULONG clen, UBYTE out[20])
{
	UBYTE k[64], pad[64], inner[20];
	struct sha1 c;
	int i;

	for (i = 0; i < 64; i++)
		k[i] = 0;
	if (klen > 64)
		fakeap_sha1(key, klen, k);
	else
		for (i = 0; i < (int)klen; i++)
			k[i] = key[i];
	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x36;
	sha1_init(&c);
	sha1_update(&c, pad, 64);
	if (alen)
		sha1_update(&c, a, alen);
	if (blen)
		sha1_update(&c, b, blen);
	if (clen)
		sha1_update(&c, cp, clen);
	sha1_final(&c, inner);
	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x5c;
	sha1_init(&c);
	sha1_update(&c, pad, 64);
	sha1_update(&c, inner, 20);
	sha1_final(&c, out);
}

/* ------------------------------------------------------------------------
 * PBKDF2-HMAC-SHA1, 4096 iterations, 32 bytes (RFC 8018 5.2; IEEE
 * 802.11-2016 J.4)
 */

void
fakeap_pmk(const char *pass, const char *ssid, UBYTE pmk[32])
{
	UBYTE u[20], t[20], idx[4];
	ULONG plen = 0, slen = 0;
	int blk, i, j;

	while (pass[plen])
		plen++;
	while (ssid[slen])
		slen++;
	for (blk = 1; blk <= 2; blk++) {
		idx[0] = idx[1] = idx[2] = 0;
		idx[3] = blk;
		fakeap_hmac_sha1((const UBYTE *)pass, plen, (const UBYTE *)ssid,
		    slen, idx, 4, NULL, 0, u);
		for (j = 0; j < 20; j++)
			t[j] = u[j];
		for (i = 1; i < 4096; i++) {
			fakeap_hmac_sha1((const UBYTE *)pass, plen, u, 20,
			    NULL, 0, NULL, 0, u);
			for (j = 0; j < 20; j++)
				t[j] ^= u[j];
		}
		for (j = 0; j < 20 && (blk - 1) * 20 + j < 32; j++)
			pmk[(blk - 1) * 20 + j] = t[j];
	}
}

/* ------------------------------------------------------------------------
 * PRF (IEEE 802.11-2016 12.7.1.2): HMAC-SHA1(K, A || 0 || B || i)
 */

void
fakeap_prf(const UBYTE *k, ULONG klen, const char *label, const UBYTE *b,
    ULONG blen, UBYTE *out, ULONG outlen)
{
	UBYTE abuf[64], h[20], cnt;
	ULONG alen = 0, done = 0, n;

	while (label[alen]) {
		abuf[alen] = label[alen];
		alen++;
	}
	abuf[alen++] = 0;
	for (cnt = 0; done < outlen; cnt++) {
		fakeap_hmac_sha1(k, klen, abuf, alen, b, blen, &cnt, 1, h);
		for (n = 0; n < 20 && done < outlen; n++)
			out[done++] = h[n];
	}
}

/* ------------------------------------------------------------------------
 * AES-128 encryption (FIPS 197) and AES key wrap (RFC 3394)
 */

static const UBYTE sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
	0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
	0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
	0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
	0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
	0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
	0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
	0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
	0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
	0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
	0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
	0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
	0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
	0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
	0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
	0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
	0xb0, 0x54, 0xbb, 0x16
};

static UBYTE
xtime(UBYTE x)
{

	return (UBYTE)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

/* the 11 round keys of a 128-bit key (FIPS 197 5.2) */
static void
aes_expand(const UBYTE key[16], UBYTE rk[176])
{
	UBYTE t[4], rcon = 1, x;
	int i, j;

	for (i = 0; i < 16; i++)
		rk[i] = key[i];
	for (i = 16; i < 176; i += 4) {
		for (j = 0; j < 4; j++)
			t[j] = rk[i - 4 + j];
		if (i % 16 == 0) {
			x = t[0];
			t[0] = sbox[t[1]] ^ rcon;
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[x];
			rcon = xtime(rcon);
		}
		for (j = 0; j < 4; j++)
			rk[i + j] = rk[i - 16 + j] ^ t[j];
	}
}

/* one block (FIPS 197 5.1; the state column by column, as the input) */
static void
aes_encrypt(const UBYTE rk[176], UBYTE s[16])
{
	UBYTE t[16], a, b, c, d;
	int r, i;

	for (i = 0; i < 16; i++)
		s[i] ^= rk[i];
	for (r = 1; r <= 10; r++) {
		/* SubBytes and ShiftRows: row i moves left by i */
		for (i = 0; i < 16; i++)
			t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];
		/* MixColumns, not in the last round */
		for (i = 0; i < 16; i += 4) {
			a = t[i];
			b = t[i + 1];
			c = t[i + 2];
			d = t[i + 3];
			if (r < 10) {
				s[i] = xtime(a) ^ (xtime(b) ^ b) ^ c ^ d;
				s[i + 1] = a ^ xtime(b) ^ (xtime(c) ^ c) ^ d;
				s[i + 2] = a ^ b ^ xtime(c) ^ (xtime(d) ^ d);
				s[i + 3] = (xtime(a) ^ a) ^ b ^ c ^ xtime(d);
			} else {
				s[i] = a;
				s[i + 1] = b;
				s[i + 2] = c;
				s[i + 3] = d;
			}
		}
		for (i = 0; i < 16; i++)
			s[i] ^= rk[16 * r + i];
	}
}

void
fakeap_aes_encrypt(const UBYTE key[16], UBYTE block[16])
{
	UBYTE rk[176];

	aes_expand(key, rk);
	aes_encrypt(rk, block);
}

/* RFC 3394 2.2.1: n 64-bit blocks of p; out has n + 1 */
void
fakeap_keywrap(const UBYTE kek[16], const UBYTE *p, int n, UBYTE *out)
{
	UBYTE rk[176], b[16];
	int i, j, k;
	ULONG t;

	aes_expand(kek, rk);
	for (i = 0; i < 8; i++)
		out[i] = 0xa6;
	for (i = 0; i < 8 * n; i++)
		out[8 + i] = p[i];
	for (j = 0; j <= 5; j++)
		for (i = 1; i <= n; i++) {
			for (k = 0; k < 8; k++) {
				b[k] = out[k];
				b[8 + k] = out[8 * i + k];
			}
			aes_encrypt(rk, b);
			t = (ULONG)(n * j + i);
			for (k = 0; k < 8; k++)
				out[k] = b[k];
			out[7] ^= t;
			out[6] ^= t >> 8;
			out[5] ^= t >> 16;
			out[4] ^= t >> 24;
			for (k = 0; k < 8; k++)
				out[8 * i + k] = b[8 + k];
		}
}

/* ------------------------------------------------------------------------
 * the 4-way handshake, authenticator side
 */

/* the RSN element the network is seen with (scan) and puts in message
   3: version 1, group CCMP, one pairwise CCMP, one AKM PSK, capabilities
   0 (IEEE 802.11-2016 9.4.2.25; suite 00-0F-AC:4 CCMP, 00-0F-AC:2 PSK) */
const UBYTE fakeap_rsn_ie[22] = {
	0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
	0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,
	0x00, 0x00
};

/* key information bits (IEEE 802.11-2016 12.7.2, Figure 12-33) */
#define	KI_VER2		0x0002		/* HMAC-SHA1 MIC, AES key wrap */
#define	KI_PAIRWISE	0x0008
#define	KI_INSTALL	0x0040
#define	KI_ACK		0x0080
#define	KI_MIC		0x0100
#define	KI_SECURE	0x0200
#define	KI_ENCDATA	0x1000

/* offsets in the frame: the EAPOL header (4 bytes), then the key
   descriptor: type 1, info 2, key length 2, replay counter 8, nonce 32,
   IV 16, RSC 8, reserved 8, MIC 16, data length 2, data */
#define	O_INFO		5
#define	O_REPLAY	9
#define	O_NONCE		17
#define	O_MIC		81
#define	O_DLEN		97
#define	O_DATA		99

/* message 3's key data: the RSN element (22) and the GTK KDE (24),
   46 bytes, padded to 48; wrapped, 8 more */
#define	KEYDATA_MAX	48

static void
put16(UBYTE *p, UWORD v)
{

	p[0] = v >> 8;
	p[1] = v;
}

static UWORD
get16(const UBYTE *p)
{

	return (UWORD)(p[0] << 8 | p[1]);
}

static int
cmp(const UBYTE *a, const UBYTE *b, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return a[i] < b[i] ? -1 : 1;
	return 0;
}

/* a frame with info, replay counter, ANonce and key data; MIC with kck
   if not NULL */
static ULONG
build(struct fakeap *ap, UWORD info, const UBYTE *data, int dlen,
    const UBYTE *kck, UBYTE *out)
{
	UBYTE mic[20];
	int i, len = O_DATA + dlen;

	for (i = 0; i < len; i++)
		out[i] = 0;
	out[0] = 2;			/* 802.1X-2004 */
	out[1] = 3;			/* EAPOL-Key */
	put16(out + 2, len - 4);
	out[4] = 2;			/* RSN key descriptor */
	put16(out + O_INFO, info);
	put16(out + 7, 16);		/* CCMP key length */
	out[O_REPLAY + 7] = ap->replay;
	for (i = 0; i < 32; i++)
		out[O_NONCE + i] = ap->anonce[i];
	put16(out + O_DLEN, dlen);
	for (i = 0; i < dlen; i++)
		out[O_DATA + i] = data[i];
	if (kck) {
		fakeap_hmac_sha1(kck, 16, out, len, NULL, 0, NULL, 0, mic);
		for (i = 0; i < 16; i++)
			out[O_MIC + i] = mic[i];
	}
	return len;
}

/* the MIC of a received frame, with its MIC field taken as zero */
static int
mic_ok(const UBYTE *kck, const UBYTE *f, ULONG len)
{
	UBYTE copy[FAKEAP_FRAME_MAX], mic[20];
	ULONG i;

	if (len > sizeof(copy))
		return 0;
	for (i = 0; i < len; i++)
		copy[i] = f[i];
	for (i = 0; i < 16; i++)
		copy[O_MIC + i] = 0;
	fakeap_hmac_sha1(kck, 16, copy, len, NULL, 0, NULL, 0, mic);
	return cmp(mic, f + O_MIC, 16) == 0;
}

void
fakeap_init(struct fakeap *ap, const char *pass, const char *ssid,
    const UBYTE aa[6], ULONG seed)
{
	int i;

	fakeap_pmk(pass, ssid, ap->pmk);
	for (i = 0; i < 6; i++)
		ap->aa[i] = aa[i];
	ap->seed = seed;
	ap->state = FAKEAP_IDLE;
	ap->micfail = 0;
}

/* message 1 to the supplicant spa: the start of a handshake */
ULONG
fakeap_msg1(struct fakeap *ap, const UBYTE spa[6], UBYTE *out)
{
	UBYTE h[20];
	int i;

	for (i = 0; i < 6; i++)
		ap->spa[i] = spa[i];
	/* (a fresh nonce: SHA-1 of a counter, enough for a test) */
	ap->seed++;
	fakeap_sha1((const UBYTE *)&ap->seed, sizeof(ap->seed), h);
	for (i = 0; i < 20; i++)
		ap->anonce[i] = h[i];
	fakeap_sha1(h, 20, h);
	for (i = 0; i < 12; i++)
		ap->anonce[20 + i] = h[i];
	ap->replay = 1;
	ap->state = FAKEAP_SENT1;
	return build(ap, KI_VER2 | KI_PAIRWISE | KI_ACK, NULL, 0, NULL, out);
}

/*
 * A frame from the supplicant.  Message 2 with the right MIC: message 3
 * in out, returns its length.  Message 4 with the right MIC: state
 * FAKEAP_DONE, 0.  Anything else, or a wrong MIC (another passphrase):
 * 0, and the state stays.
 */
ULONG
fakeap_rx(struct fakeap *ap, const UBYTE *f, ULONG len, UBYTE *out)
{
	UBYTE b[76], gtkd[KEYDATA_MAX], wrapped[KEYDATA_MAX + 8];
	UWORD info;
	int i, n;

	if (len < O_DATA || f[1] != 3 || f[4] != 2)
		return 0;
	info = get16(f + O_INFO);
	if (!(info & KI_MIC) || f[O_REPLAY + 7] != ap->replay)
		return 0;
	if (ap->state == FAKEAP_SENT1 && !(info & KI_SECURE)) {
		/* the PTK from both addresses and both nonces (12.7.1.3) */
		const UBYTE *snonce = f + O_NONCE;

		if (cmp(ap->aa, ap->spa, 6) < 0) {
			for (i = 0; i < 6; i++) {
				b[i] = ap->aa[i];
				b[6 + i] = ap->spa[i];
			}
		} else
			for (i = 0; i < 6; i++) {
				b[i] = ap->spa[i];
				b[6 + i] = ap->aa[i];
			}
		if (cmp(ap->anonce, snonce, 32) < 0) {
			for (i = 0; i < 32; i++) {
				b[12 + i] = ap->anonce[i];
				b[44 + i] = snonce[i];
			}
		} else
			for (i = 0; i < 32; i++) {
				b[12 + i] = snonce[i];
				b[44 + i] = ap->anonce[i];
			}
		fakeap_prf(ap->pmk, 32, "Pairwise key expansion", b, 76,
		    ap->ptk, 48);
		if (!mic_ok(ap->ptk, f, len)) {
			ap->micfail++;
			return 0;
		}
		/* message 3: the RSN element and the GTK KDE (12.7.2,
		   Table 12-6: 00-0F-AC:1, key ID 1), padded with 0xdd and
		   zeros to whole 8-byte blocks, wrapped with the KEK */
		n = 0;
		for (i = 0; i < (int)sizeof(fakeap_rsn_ie); i++)
			gtkd[n++] = fakeap_rsn_ie[i];
		gtkd[n++] = 0xdd;
		gtkd[n++] = 22;
		gtkd[n++] = 0x00;
		gtkd[n++] = 0x0f;
		gtkd[n++] = 0xac;
		gtkd[n++] = 0x01;
		gtkd[n++] = 0x01;		/* key ID 1 */
		gtkd[n++] = 0x00;
		for (i = 0; i < 16; i++)
			gtkd[n++] = (UBYTE)(0x40 + i);	/* the GTK */
		if (n % 8)
			gtkd[n++] = 0xdd;
		while (n % 8)
			gtkd[n++] = 0;
		fakeap_keywrap(ap->ptk + 16, gtkd, n / 8, wrapped);
		ap->replay = 2;
		ap->state = FAKEAP_SENT3;
		return build(ap, KI_VER2 | KI_PAIRWISE | KI_INSTALL | KI_ACK |
		    KI_MIC | KI_SECURE | KI_ENCDATA, wrapped, n + 8, ap->ptk,
		    out);
	}
	if (ap->state == FAKEAP_SENT3 && (info & KI_SECURE)) {
		if (!mic_ok(ap->ptk, f, len)) {
			ap->micfail++;
			return 0;
		}
		ap->state = FAKEAP_DONE;
		return 0;
	}
	return 0;
}

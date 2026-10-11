/*
 * The crypto of src/test/fakeap.c against published test vectors:
 *   SHA-1("abc")                FIPS 180-2 appendix A.1:
 *                               a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d
 *   HMAC-SHA1                   RFC 2202 section 3, test case 2 (key
 *                               "Jefe"): effcdf6a e5eb2fa2 d27416d5
 *                               f184df9c 259a7c79
 *   PBKDF2 "password" / "IEEE"  IEEE 802.11-2016 J.4.2: f42c6fc5 2df0ebef
 *                               9ebb4b90 b38a5f90 2e83fe1b 135a70e2
 *                               3aed762e 9710a12e
 *   AES-128                     FIPS 197 appendix C.1: 69c4e0d8 6a7b0430
 *                               d8cdb780 70b4c55a
 *   AES key wrap                RFC 3394 section 4.1: 1fa68b0a 8112b447
 *                               aef34bd8 fb5a7b82 9d3e8623 71d2cfe5
 *   PRF-384                     no published vector: the value of
 *                               Python's hmac/hashlib modules for the
 *                               PMK of "fakepassword" / "FakeNet" and
 *                               B = bytes 0 to 75
 *
 *   tools/build_amiga.sh src/test/fakeaptest.c build/fakeaptest \
 *       src/test/fakeap.c
 *   python -I tools/run_emu.py build/fakeaptest --timeout 300
 */
#include "emuenv.h"

#include "fakeap.h"

static int
hexeq(const UBYTE *p, const char *hex, int n)
{
	int i, v;

	for (i = 0; i < n; i++) {
		char a = hex[2 * i], b = hex[2 * i + 1];

		v = ((a <= '9' ? a - '0' : a - 'a' + 10) << 4) |
		    (b <= '9' ? b - '0' : b - 'a' + 10);
		if (p[i] != v)
			return 0;
	}
	return 1;
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	UBYTE h[20], pmk[32], ptk[48], blk[16], b[76], wrapped[24];
	static const UBYTE key[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
	    12, 13, 14, 15 };
	static const UBYTE plain[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
	    0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
	int i;

	if (!start("fakeaptest"))
		return RETURN_FAIL;
	fakeap_sha1((const UBYTE *)"abc", 3, h);
	check("SHA-1 abc", hexeq(h, "a9993e364706816aba3e25717850c26c9cd0d89d",
	    20));
	fakeap_hmac_sha1((const UBYTE *)"Jefe", 4,
	    (const UBYTE *)"what do ya want for nothing?", 28, NULL, 0, NULL, 0,
	    h);
	check("HMAC-SHA1 RFC 2202 case 2",
	    hexeq(h, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", 20));
	fakeap_pmk("password", "IEEE", pmk);
	check("PBKDF2 password/IEEE", hexeq(pmk,
	    "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e",
	    32));
	for (i = 0; i < 16; i++)
		blk[i] = plain[i];
	fakeap_aes_encrypt(key, blk);
	check("AES-128 FIPS 197 C.1",
	    hexeq(blk, "69c4e0d86a7b0430d8cdb78070b4c55a", 16));
	fakeap_keywrap(key, plain, 2, wrapped);
	check("AES key wrap RFC 3394 4.1", hexeq(wrapped,
	    "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", 24));
	fakeap_pmk("fakepassword", "FakeNet", pmk);
	for (i = 0; i < 76; i++)
		b[i] = i;
	fakeap_prf(pmk, 32, "Pairwise key expansion", b, 76, ptk, 48);
	check("PRF-384", hexeq(ptk,
	    "c2a3545d5450d36b80f1ac2a79e582b547ef0886f8fda40fb289d01eb0c2698d"
	    "d723a2392aaf8bd4ac0b7b42eac95643", 48));
	return finish("fakeaptest");
}

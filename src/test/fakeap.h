/*
 * The access point side of fakewifi.device: WPA2-PSK 4-way handshake
 * (src/test/fakeap.c).  Test only.
 */
#ifndef FAKEAP_H
#define FAKEAP_H

#include <exec/types.h>

#define	FAKEAP_FRAME_MAX	256

#define	FAKEAP_IDLE	0
#define	FAKEAP_SENT1	1	/* message 1 out, waiting for 2 */
#define	FAKEAP_SENT3	3	/* message 3 out, waiting for 4 */
#define	FAKEAP_DONE	4	/* message 4 had the right MIC */

struct fakeap {
	UBYTE pmk[32];
	UBYTE ptk[48];		/* KCK 0-15, KEK 16-31, TK 32-47 */
	UBYTE aa[6], spa[6];
	UBYTE anonce[32];
	UBYTE replay;
	int state;
	int micfail;		/* frames with a wrong MIC */
	ULONG seed;
};

extern const UBYTE fakeap_rsn_ie[22];

void	fakeap_sha1(const UBYTE *, ULONG, UBYTE[20]);
void	fakeap_hmac_sha1(const UBYTE *, ULONG, const UBYTE *, ULONG,
	    const UBYTE *, ULONG, const UBYTE *, ULONG, UBYTE[20]);
void	fakeap_pmk(const char *, const char *, UBYTE[32]);
void	fakeap_prf(const UBYTE *, ULONG, const char *, const UBYTE *, ULONG,
	    UBYTE *, ULONG);
void	fakeap_aes_encrypt(const UBYTE[16], UBYTE[16]);
void	fakeap_keywrap(const UBYTE[16], const UBYTE *, int, UBYTE *);
void	fakeap_init(struct fakeap *, const char *, const char *,
	    const UBYTE[6], ULONG);
ULONG	fakeap_msg1(struct fakeap *, const UBYTE[6], UBYTE *);
ULONG	fakeap_rx(struct fakeap *, const UBYTE *, ULONG, UBYTE *);

#endif

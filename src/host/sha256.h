/*
 * SHA-256 (FIPS 180-4, https://doi.org/10.6028/NIST.FIPS.180-4), for the
 * host entropy pool (entropy.c).  Plain C with no AmigaOS dependency, so
 * it can be checked against a reference implementation on any machine.
 */
#ifndef AMIBSDNET_SHA256_H
#define AMIBSDNET_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define	SHA256_DIGEST_LEN	32
#define	SHA256_BLOCK_LEN	64

struct sha256 {
	uint32_t h[8];
	uint64_t nbytes;		/* message length so far */
	uint32_t nbuf;			/* bytes waiting in buf */
	uint8_t buf[SHA256_BLOCK_LEN];
};

void	sha256_init(struct sha256 *);
void	sha256_update(struct sha256 *, const void *, size_t);
/* writes the digest; the context must be initialised again to be reused */
void	sha256_final(struct sha256 *, uint8_t[SHA256_DIGEST_LEN]);

#endif /* AMIBSDNET_SHA256_H */

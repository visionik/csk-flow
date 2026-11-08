/* Minimal MD5, public domain (Alexander Peslyak's reference implementation). */
#ifndef CSK_FLOW_MD5_H
#define CSK_FLOW_MD5_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
	uint32_t lo, hi;
	uint32_t a, b, c, d;
	unsigned char buffer[64];
	uint32_t block[16];
} md5_ctx;

void md5_init(md5_ctx *ctx);
void md5_update(md5_ctx *ctx, const void *data, size_t size);
void md5_final(unsigned char result[16], md5_ctx *ctx);

/* Convenience: MD5 of a whole file. Returns 0 and fills out[16] and *size. */
int md5_file(const char *path, unsigned char out[16], long *size);

#endif

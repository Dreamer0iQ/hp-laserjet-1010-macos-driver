#ifndef DELTAPLUS_H
#define DELTAPLUS_H

#include <stdint.h>

/* FastRaster compression ids (25-byte block header, bytes 4..5). */
#define FR_COMP_RAW        2
#define FR_COMP_DELTAPLUS  24

/* Rows per FastRaster block (INDY_STRIP_HEIGHT in HPLIP). */
#define FR_BAND_HEIGHT     128

/* Horizontal "halftone distance" HPLIP always passes to the encoder. */
#define FR_HT_DIST         16

/*
 * Compress `height` rows of `row_width` bytes each (contiguous in `in`).
 * The seed row for the first row is all zeros, as in HPLIP.
 * On entry *outlen is the capacity of `out`; on success it is set to the
 * number of bytes written and 1 is returned. Returns 0 if `out` is too small.
 */
int dp_compress(uint8_t *out, uint32_t *outlen, const uint8_t *in,
                uint32_t row_width, uint32_t height, uint32_t ht_dist);

#endif

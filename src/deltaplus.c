/*
 * deltaplus.c - HP "DeltaPlus" encoder (FastRaster compression method 24).
 *
 * C port of ModeDeltaPlus::compress() from HPLIP (prnt/hpcups/ModeDeltaPlus.cpp).
 * Output is byte-for-byte identical to HPLIP's encoder; the only behavioural
 * change is that buffer overflow now fails cleanly instead of writing through
 * a NULL pointer.
 *
 *   Copyright (c) 1996 - 2015, HP Co.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the name of HP nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR IMPLIED
 *   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *   NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PATENT INFRINGEMENT; PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *   OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *   THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Command byte layout:  R LL SS CCC
 *   R   = 1 run / 0 literal
 *   LL  = where the first pixel comes from: 0 explicit byte, 1 north-east
 *         (seed[col+1]), 2 north-west (seed[col-1]), 3 last explicit colour
 *   SS  = bytes copied from the seed row first (3 = extended count follows)
 *   CCC = run length - 2 / extra literal bytes (7 = extended count follows)
 */

#include <string.h>
#include "deltaplus.h"

/* Hardware literal buffer limit (see HPLIP). */
#define LITERAL_LIMIT 511

static uint8_t *encode_count(uint8_t *p, const uint8_t *end, uint32_t count, uint32_t over)
{
    if (count < over)
        return p;
    count -= over;
    if (count <= 253) {
        if (p >= end) return NULL;
        *p++ = (uint8_t) count;
    } else if (count <= 254 + 255) {
        if (p + 1 >= end) return NULL;
        *p++ = 0xFE;
        *p++ = (uint8_t) (count - 254);
    } else {
        count -= 255;
        if (count > 65535 || p + 2 >= end) return NULL;
        *p++ = 0xFF;
        *p++ = (uint8_t) (count >> 8);
        *p++ = (uint8_t) (count & 0xFF);
    }
    return p;
}

static uint8_t *encode_header(uint8_t *p, const uint8_t *end, uint32_t isrun,
                              uint32_t location, uint32_t seedrow_count,
                              uint32_t run_count, uint8_t new_color)
{
    uint32_t byte = (isrun << 7) | (location << 5);

    byte |= (seedrow_count > 2 ? 3u : seedrow_count) << 3;
    byte |= run_count > 6 ? 7u : run_count;

    if (p >= end) return NULL;
    *p++ = (uint8_t) byte;

    if ((p = encode_count(p, end, seedrow_count, 3)) == NULL)
        return NULL;
    if (location == 0) {
        if (p >= end) return NULL;
        *p++ = new_color;
    }
    return encode_count(p, end, run_count, 7);
}

int dp_compress(uint8_t *out, uint32_t *outlen, const uint8_t *in,
                uint32_t row_width, uint32_t height, uint32_t ht)
{
    static const uint8_t zero_seed[8192];
    const uint8_t *end = out + *outlen;
    const uint8_t *seed = zero_seed;
    uint8_t *p = out;
    uint8_t new_color = 0xFF;

    if (ht > 32 || row_width > sizeof(zero_seed))
        return 0;
    if (ht < 1)
        ht = 1;

    for (uint32_t row = 0; row < height; row++) {
        const uint8_t *cur = in + (size_t) row * row_width;
        uint32_t col = 0;

        while (col < row_width) {
            uint32_t seedrow_count = 0, location;

            while (col < row_width && cur[col] == seed[col]) {
                seedrow_count++;
                col++;
            }

            if (col == row_width) {
                /* rest of the row equals the seed row: "fake run" */
                p = encode_header(p, end, 1, 1, seedrow_count, 0, 0);
                if (p == NULL) return 0;
                break;
            }

            if (col + 1 < row_width && cur[col] == seed[col + 1])
                location = 1;
            else if (col > 0 && cur[col] == seed[col - 1])
                location = 2;
            else if (cur[col] == new_color)
                location = 3;
            else {
                location = 0;
                new_color = cur[col];
            }

            if (col + 1 < row_width && col + 1 >= ht && cur[col + 1 - ht] == cur[col + 1]) {
                /* run: each byte repeats the one `ht` bytes to the left */
                uint32_t run_count = 0;
                col++;
                while (col + 1 < row_width && cur[col + 1 - ht] == cur[col + 1]) {
                    run_count++;
                    col++;
                }
                col++;
                p = encode_header(p, end, 1, location, seedrow_count, run_count, new_color);
                if (p == NULL) return 0;
            } else {
                uint32_t replacement_count = 0;
                const uint8_t *bytes = cur + col + 1;
                col++;
                while (col + 1 < row_width) {
                    /* break wherever a seed copy or run would save a byte */
                    if ((cur[col] == seed[col] &&
                         (cur[col + 1] == seed[col + 1] ||
                          cur[col + 1] == seed[col] ||
                          cur[col + 1] == new_color ||
                          (col + 2 < row_width &&
                           ((col + 2 >= ht && cur[col + 2 - ht] == cur[col + 2]) ||
                            cur[col + 1] == seed[col + 2])))) ||
                        (cur[col] != seed[col] &&
                         col + 1 >= ht &&
                         cur[col + 1 - ht] == cur[col + 1] &&
                         ((col + 2 < row_width && col + 2 >= ht &&
                           cur[col + 2 - ht] == cur[col + 2]) ||
                          cur[col] == seed[col + 1] ||
                          cur[col] == seed[col - 1] ||
                          cur[col] == new_color)))
                        break;
                    if (replacement_count >= LITERAL_LIMIT)
                        break;
                    replacement_count++;
                    col++;
                }
                if (col + 1 == row_width) {
                    replacement_count++;
                    col++;
                }
                p = encode_header(p, end, 0, location, seedrow_count, replacement_count, new_color);
                if (p == NULL || p + replacement_count > end) return 0;
                memcpy(p, bytes, replacement_count);
                p += replacement_count;
            }
        }
        seed = cur;
    }

    *outlen = (uint32_t) (p - out);
    return 1;
}

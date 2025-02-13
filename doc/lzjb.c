/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 */

/*
 * We keep our own copy of this algorithm for 3 main reasons:
 *    1. If we didn't, anyone modifying common/os/compress.c would
 *         directly break our on disk format
 *    2. Our version of lzjb does not have a number of checks that the
 *         common/os version needs and uses
 *    3. We initialize the lempel to ensure deterministic results,
 *       so that identical blocks can always be deduplicated.
 * In particular, we are adding the "feature" that compress() can
 * take a destination buffer size and returns the compressed length, or the
 * source length if compression would overflow the destination buffer.
 */

#include <sys/types.h>
#include <stdio.h>
#include <errno.h>

typedef unsigned char uchar_t;
#define NBBY 8

#define    MATCH_BITS    6
#define    MATCH_MIN    3
#define    MATCH_MAX    ((1 << MATCH_BITS) + (MATCH_MIN - 1))
#define    OFFSET_MASK    ((1 << (16 - MATCH_BITS)) - 1)
#define    LEMPEL_SIZE    1024

/*ARGSUSED*/
size_t
lzjb_compress(void *s_start, void *d_start, size_t s_len, size_t d_len, int n)
{
    uchar_t *src = s_start;
    uchar_t *dst = d_start;
    uchar_t *cpy, *copymap;
    int copymask = 1 << (NBBY - 1);
    int mlen, offset, hash;
    uint16_t *hp;
    uint16_t lempel[LEMPEL_SIZE] = { 0 };

    while (src < (uchar_t *)s_start + s_len) {
        if ((copymask <<= 1) == (1 << NBBY)) {
            if (dst >= (uchar_t *)d_start + d_len - 1 - 2 * NBBY)
                return (s_len);
            copymask = 1;
            copymap = dst;
            *dst++ = 0;
        }
        if (src > (uchar_t *)s_start + s_len - MATCH_MIN) {
            *dst++ = *src++;
            continue;
        }
        hash = (src[0] << 16) + (src[1] << 8) + src[2];
        hash ^= hash >> 9; /* CSA TWEAK */
        hash += hash >> 5;
        hash ^= src[0]; /* CSA TWEAK */
        hp = &lempel[hash & (LEMPEL_SIZE - 1)];
        offset = (intptr_t)(src - *hp) & OFFSET_MASK;
        *hp = (uint16_t)(uintptr_t)src;
        cpy = src - offset;
        if (offset == 0) { cpy -= OFFSET_MASK + 1; } /* CSA TWEAK */
        if (cpy >= (uchar_t *)s_start && cpy != src &&
            src[0] == cpy[0] && src[1] == cpy[1] && src[2] == cpy[2]) {
            *copymap |= copymask;
            for (mlen = MATCH_MIN; mlen < MATCH_MAX; mlen++) {
                if (src + mlen >= (uchar_t*)s_start + s_len)
                    break; /* CSA TWEAK */
                if (src[mlen] != cpy[mlen])
                    break;
            }
            *dst++ = ((mlen - MATCH_MIN) << (NBBY - MATCH_BITS)) |
                (offset >> NBBY);
            *dst++ = (uchar_t)offset;
            src += mlen;
        } else {
            *dst++ = *src++;
        }
    }
    return (dst - (uchar_t *)d_start);
}

/*ARGSUSED*/
int
lzjb_decompress(void *s_start, void *d_start, size_t s_len, size_t d_len, int n)
{
    uchar_t *src = s_start;
    uchar_t *dst = d_start;
    uchar_t *d_end = (uchar_t *)d_start + d_len;
    uchar_t *cpy, copymap = 0;
    int copymask = 1 << (NBBY - 1);

    while (dst < d_end) {
        if ((copymask <<= 1) == (1 << NBBY)) {
            copymask = 1;
            copymap = *src++;
        }
        if (copymap & copymask) {
            int mlen = (src[0] >> (NBBY - MATCH_BITS)) + MATCH_MIN;
            int offset = ((src[0] << NBBY) | src[1]) & OFFSET_MASK;
#if (0)
            printf("mlen := %02i, offset := %04i\n", mlen, offset);
#endif

            if (offset==0) { offset=OFFSET_MASK+1; } /* CSA TWEAK */
            src += 2;
            if ((cpy = dst - offset) < (uchar_t *)d_start)
                return (-1);
            while (--mlen >= 0 && dst < d_end)
                *dst++ = *cpy++;
        } else {
            *dst++ = *src++;
        }
    }
    return (0);
}

typedef int (*fops_read_t) (uchar_t *dst, void *fops_ctx, const size_t len, const size_t offset);
typedef int (*fops_write_t) (void *fops_ctx, const uchar_t *src, const size_t len, const size_t offset);

typedef struct __lz_struct_ctx_t
{
    fops_read_t        sread;
    fops_read_t        dread;
    fops_write_t    dwrite;
    void            *sctx;
    void            *dctx;

    size_t            scnt;
    size_t            dcnt;
    size_t            dlen;

    uchar_t            copymap;
    uchar_t            copymask;
} lz_ctx_t;

int lzjb_decompress_ext(lz_ctx_t *ctx, size_t len)
{
    int rc = 0;

    if (ctx->dcnt < ctx->dlen) {
        uchar_t src_byte[2u], dst_byte;
        size_t scnt = ctx->scnt; size_t dcnt = ctx->dcnt;
        size_t cnt = 0;

        int copymask = ctx->copymask;
        uchar_t copymap = ctx->copymap;

        while ((dcnt < ctx->dlen) && (cnt < len)) {
            if ((copymask <<= 1) == (1 << NBBY)) {
                copymask = 1;
                ctx->sread(&src_byte[0], ctx->sctx, 1U, scnt++);
                copymap = src_byte[0];
            }
            if (copymap & copymask) {
                ctx->sread(&src_byte[0], ctx->sctx, 2U, scnt); scnt += 2;
                int mlen = (src_byte[0] >> (NBBY - MATCH_BITS)) + MATCH_MIN;
                int offset = ((src_byte[0] << NBBY) | src_byte[1]) & OFFSET_MASK;

                if (offset==0) { offset=OFFSET_MASK+1; } /* CSA TWEAK */

                int cpy_idx = dcnt - offset;
                if (cpy_idx > 0) {
                    while (--mlen >= 0 && (dcnt < ctx->dlen)) {
                        ctx->dread(&dst_byte, ctx->dctx, 1U, cpy_idx++);
                        ctx->dwrite(ctx->dctx, &dst_byte, 1U, dcnt++); cnt++;
                    }
                }
                else {
                    rc = -EINVAL;
                    break;
                }
            } else {
                ctx->sread(&src_byte[0], ctx->sctx, 1U, scnt++);
                ctx->dwrite(ctx->dctx, &src_byte[0], 1U, dcnt++); cnt++;
            }
        }

        ctx->scnt = scnt;
        ctx->dcnt = dcnt;
        ctx->copymap = copymap;
        ctx->copymask = copymask;

        if (!rc) rc = cnt;
    }

    else {
        rc = -EOF;
    }

    return (rc);
}

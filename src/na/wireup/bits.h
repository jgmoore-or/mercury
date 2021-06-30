/* * Copyright (c) 1991, 1993
 *      The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Berkeley Software Design, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *      @(#)cdefs.h     8.8 (Berkeley) 1/9/95
 */

#ifndef BITS_H_
#define BITS_H_

#include <sys/param.h>

/* Bitmask-manipulation macros derived from __BIT, __BITS, et cetera, in
 * NetBSD.
 */

/* BIT(n): nth bit, where BIT(0) == 0x1. */
#define BIT(n)      \
    (((uintmax_t)(n) >= NBBY * sizeof(uintmax_t)) ? 0 : \
        ((uintmax_t)1 << (uintmax_t)((n) & (NBBY * sizeof(uintmax_t) - 1))))

    /* BITS(m, n): bits m through n, m < n. */
#define BITS(m, n)        \
            ((BIT(MAX((m), (n)) + 1) - 1) ^ (BIT(MIN((m), (n))) - 1))

            /* find least significant bit that is set */
#define LOWEST_SET_BIT(mask) ((((mask) - 1) & (mask)) ^ (mask))

#define PRIuBIT       PRIuMAX
#define PRIuBITS      PRIuBIT

#define PRIxBIT       PRIxMAX
#define PRIxBITS      PRIxBIT

#define SHIFTOUT(x, mask) (((x) & (mask)) / LOWEST_SET_BIT(mask))
#define SHIFTIN(x, mask) ((x) * LOWEST_SET_BIT(mask))
#define SHIFTOUT_MASK(mask) SHIFTOUT((mask), (mask))

#endif /* BITS_H_ */

#ifndef LIBMDF_PAGER_RATIO_H
#define LIBMDF_PAGER_RATIO_H

#include <limits.h>
#include <stddef.h>

/* Return floor(offset * current_len / previous_len) without materializing the
 * product.  offset is a position in previous_len, so the quotient never
 * exceeds current_len; the binary long division therefore stays in size_t on
 * both 32- and 64-bit targets. */
static size_t mdf_pager_scale_anchor(size_t offset, size_t current_len, size_t previous_len)
{
    size_t quotient;
    size_t remainder;
    size_t bit;

    if (previous_len == 0) return offset;
    if (offset >= previous_len) return current_len;
    quotient = 0;
    remainder = 0;
    bit = (size_t)1 << (sizeof(size_t) * CHAR_BIT - 1);
    while (bit != 0) {
        quotient *= 2;
        if (remainder >= previous_len - remainder) {
            remainder -= previous_len - remainder;
            quotient++;
        } else {
            remainder += remainder;
        }
        if ((current_len & bit) != 0) {
            if (offset >= previous_len - remainder) {
                remainder = offset - (previous_len - remainder);
                quotient++;
            } else {
                remainder += offset;
            }
        }
        bit >>= 1;
    }
    return quotient;
}

#endif

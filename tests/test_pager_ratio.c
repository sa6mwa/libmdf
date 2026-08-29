#include <stdio.h>

#include "../src/pager_ratio.h"

static int expect(size_t actual, size_t expected, const char *name)
{
    if (actual == expected) return 0;
    fprintf(stderr, "%s: expected %lu, got %lu\n", name,
            (unsigned long)expected, (unsigned long)actual);
    return 1;
}

int main(void)
{
    int fails;

    fails = 0;
    fails += expect(mdf_pager_scale_anchor(3, 10, 7), 4,
                    "scales fractional positions");
    /* 65536 * 65536 overflows a 32-bit size_t, but this is a valid resize
     * anchor ratio and must remain near the end of the document. */
    fails += expect(mdf_pager_scale_anchor(65536, 65536, 65537), 65535,
                    "avoids 32-bit resize anchor overflow");
    fails += expect(mdf_pager_scale_anchor(17, 99, 17), 99,
                    "scales terminal anchor to current length");
    return fails == 0 ? 0 : 1;
}

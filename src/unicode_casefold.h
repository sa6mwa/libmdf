#ifndef LIBMDF_UNICODE_CASEFOLD_H
#define LIBMDF_UNICODE_CASEFOLD_H

#include <stddef.h>

/* Returns the Unicode 16.0 default case-folded code-point sequence for cp. */
size_t mdf_unicode_casefold(unsigned long cp, unsigned long folded[3]);

#endif

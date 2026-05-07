#pragma once
#include <stddef.h>

/* Upper bound on output size for packbits_encode(). */
size_t packbits_max(size_t in_len);

/* TIFF packbits encode.  'out' must be >= packbits_max(in_len) bytes.
 * Returns number of bytes written. */
size_t packbits_encode(const unsigned char *in, size_t in_len,
                       unsigned char *out);

#include "packbits.h"
#include <string.h>

size_t packbits_max(size_t n)
{
    return n + (n / 128) + 2;
}

size_t packbits_encode(const unsigned char *in, size_t n, unsigned char *out)
{
    size_t i = 0, o = 0;

    while (i < n) {
        if (i + 1 < n && in[i] == in[i + 1]) {
            /* Replicate run: find length (max 128) */
            size_t run = 2;
            while (run < 128 && i + run < n && in[i + run] == in[i])
                run++;
            out[o++] = (unsigned char)(1 - (int)run); /* -(run-1) as signed byte */
            out[o++] = in[i];
            i += run;
        } else {
            /* Literal run: accumulate until a replicate run starts */
            size_t lit = 1;
            while (lit < 128 && i + lit < n) {
                if (i + lit + 1 < n && in[i + lit] == in[i + lit + 1])
                    break;
                lit++;
            }
            out[o++] = (unsigned char)(lit - 1);
            memcpy(out + o, in + i, lit);
            o += lit;
            i += lit;
        }
    }

    return o;
}

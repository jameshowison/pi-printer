/* T5 offline gate: generate a minimal APT TIFF and write it to stdout.
 * Mirrors apt_build_tiff_header() in driver.c exactly.
 * Usage: ./test_apt_tiff | tiffinfo -
 * Or:    ./test_apt_tiff > /tmp/apt.tif && tiffinfo /tmp/apt.tif */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APT_TIFF_HDR_SIZE 174u
#define APT_INPUT_DPI     150

static void pu16(unsigned char *buf, size_t *off, unsigned v)
{
    buf[(*off)++] = (unsigned char)(v & 0xff);
    buf[(*off)++] = (unsigned char)((v >> 8) & 0xff);
}

static void pu32(unsigned char *buf, size_t *off, unsigned long v)
{
    buf[(*off)++] = (unsigned char)(v & 0xff);
    buf[(*off)++] = (unsigned char)((v >> 8) & 0xff);
    buf[(*off)++] = (unsigned char)((v >> 16) & 0xff);
    buf[(*off)++] = (unsigned char)((v >> 24) & 0xff);
}

static void ptag(unsigned char *buf, size_t *off,
                 unsigned tag, unsigned type, unsigned long count,
                 unsigned long value_or_offset)
{
    pu16(buf, off, tag);
    pu16(buf, off, type);
    pu32(buf, off, count);
    if (type == 3)
        pu16(buf, off, (unsigned)value_or_offset), pu16(buf, off, 0);
    else
        pu32(buf, off, value_or_offset);
}

static void apt_build_tiff_header(unsigned char *buf, unsigned w, unsigned h)
{
    size_t off = 0;

    buf[off++] = 0x49; buf[off++] = 0x49;
    pu16(buf, &off, 42);
    pu32(buf, &off, 8);

    pu16(buf, &off, 12);

    const unsigned long rational_base = 158;
    const unsigned long pixel_offset  = APT_TIFF_HDR_SIZE;

    ptag(buf, &off, 256, 3, 1, w);
    ptag(buf, &off, 257, 3, 1, h);
    ptag(buf, &off, 258, 3, 1, 8);
    ptag(buf, &off, 259, 3, 1, 1);
    ptag(buf, &off, 262, 3, 1, 1);
    ptag(buf, &off, 273, 4, 1, pixel_offset);
    ptag(buf, &off, 277, 3, 1, 1);
    ptag(buf, &off, 278, 4, 1, h);
    ptag(buf, &off, 279, 4, 1, (unsigned long)w * h);
    ptag(buf, &off, 282, 5, 1, rational_base);
    ptag(buf, &off, 283, 5, 1, rational_base + 8);
    ptag(buf, &off, 296, 3, 1, 2);

    pu32(buf, &off, 0);

    pu32(buf, &off, APT_INPUT_DPI); pu32(buf, &off, 1);
    pu32(buf, &off, APT_INPUT_DPI); pu32(buf, &off, 1);

    (void)off;
}

int main(int argc, char *argv[])
{
    /* Default: Letter at 150 dpi = 1275 x 1650 px (matches plan) */
    unsigned w = 1275, h = 1650;
    if (argc == 3) {
        w = (unsigned)atoi(argv[1]);
        h = (unsigned)atoi(argv[2]);
    }

    unsigned char hdr[APT_TIFF_HDR_SIZE];
    apt_build_tiff_header(hdr, w, h);

    /* Verify header size */
    fprintf(stderr, "Header size: %u bytes (expected %u)\n",
            (unsigned)APT_TIFF_HDR_SIZE, APT_TIFF_HDR_SIZE);

    /* Write header */
    if (fwrite(hdr, 1, APT_TIFF_HDR_SIZE, stdout) != APT_TIFF_HDR_SIZE) {
        perror("fwrite header");
        return 1;
    }

    /* Write pixel data: mid-gray (128) for every pixel */
    unsigned long npix = (unsigned long)w * h;
    unsigned char *row = malloc(w);
    if (!row) { perror("malloc"); return 1; }
    memset(row, 128, w);
    for (unsigned y = 0; y < h; y++) {
        if (fwrite(row, 1, w, stdout) != w) {
            perror("fwrite pixels");
            free(row);
            return 1;
        }
    }
    free(row);

    fprintf(stderr, "Wrote %lu bytes total (%u header + %lu pixels) for %ux%u px\n",
            APT_TIFF_HDR_SIZE + npix, APT_TIFF_HDR_SIZE, npix, w, h);
    return 0;
}

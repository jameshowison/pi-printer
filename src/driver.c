/* Phase 2/3/4/5/6A: driver-owned streaming raster pipeline + media substitution
 * + rich job-prefix observability logging + PJL supply/status polling
 * + APT photo path (Mode 1024).
 *
 * Architecture: PAPPL owns IPP/AirPrint/USB/job lifecycle.
 * This driver owns: GS invocation, 8-bit grayscale halftoning,
 * packbits encoding, PCL/PJL byte stream, USB back-channel.
 *
 * Raster path (PDF input):
 *   Ghostscript (pgmraw, 8-bit grayscale) → popen pipe
 *       → threshold (300 dpi) or ordered dither (600 dpi)
 *       → packbits → papplDeviceWrite()
 *
 * Raster path (PWG/JPEG/PNG via PAPPL internal filter):
 *   PAPPL delivers SGRAY_8 rows → rwriteline_cb halftones them.
 *
 * Phase 3 media substitution:
 *   apply_media_substitution() runs at the top of rstartjob_cb, before
 *   pjl_params_from_options().  It rewrites options->media.size_name so
 *   that BOTH the PJL PAPER= command AND the GS -sPAPERSIZE= argument
 *   (read from options->media.size_name in pdf_filter_cb after rstartjob_cb
 *   returns) reflect the loaded paper.  GS -dFitPage scales content to fit.
 *   Vendor options loaded-paper and media-mismatch-action control behaviour.
 *
 * PI-SIDE BUILD NOTES (PAPPL 1.4.x)
 * If the compiler complains about field names in pappl_pr_driver_data_t:
 *   - "force_raster_type" — field may not exist; this driver no longer
 *     uses it (removed in Phase 2).
 *   - "identify_actions" — may be "identify_actions_supported"; check the
 *     pappl/printer.h header and rename accordingly.
 *   - source[] / type[] — if declared as "const char *" not "char[][64]",
 *     replace the strncpy calls with direct pointer assignments.
 */

#include <pappl/pappl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include "pjl.h"
#include "packbits.h"

/* ---- Per-job state ---------------------------------------------------- */

typedef struct {
    time_t         start_time;
    int            resolution;      /* effective dpi, cached in rstartjob */
    unsigned       page_width;      /* pixels; allocated on first rstartpage */
    unsigned char *halftone_buf;    /* 1-bit packed row, (page_width+7)/8 bytes */
    unsigned char *line_buf;        /* packbits output buffer */
    size_t         line_buf_size;
    char           ctx[64];         /* rich log prefix: "<name> from <host>" */
} hl5170dn_job_t;

/* ---- Phase 4: rich job-context prefix --------------------------------- */

/* Build a human-readable job context string stored in buf (≤bufsz bytes).
 * Format: "<doc-or-job-name> from <host-or-user>", control chars stripped,
 * truncated to bufsz-1.  Falls back gracefully when attributes are absent. */
static void make_job_ctx(pappl_job_t *job, char *buf, size_t bufsz)
{
    /* Name: prefer document-name, fall back to job-name, then synthesize. */
    const char *name = NULL;
    ipp_attribute_t *attr;

    attr = papplJobGetAttribute(job, "document-name");
    if (attr)
        name = ippGetString(attr, 0, NULL);
    if (!name || !*name)
        name = papplJobGetName(job);
    if (!name || !*name) {
        const char *fmt = papplJobGetFormat(job);
        if      (fmt && strstr(fmt, "pdf"))    name = "<PDF>";
        else if (fmt && strstr(fmt, "jpeg"))   name = "<photo>";
        else if (fmt && strstr(fmt, "png"))    name = "<photo>";
        else if (fmt && strstr(fmt, "raster")) name = "<raster>";
        else                                   name = "<unknown>";
    }

    /* Host: prefer job-originating-host-name, fall back to username. */
    const char *host = NULL;
    attr = papplJobGetAttribute(job, "job-originating-host-name");
    if (attr)
        host = ippGetString(attr, 0, NULL);
    if (!host || !*host)
        host = papplJobGetUsername(job);

    char tmp[128];
    if (host && *host)
        snprintf(tmp, sizeof(tmp), "%s from %s", name, host);
    else
        snprintf(tmp, sizeof(tmp), "%s", name);

    /* Strip control characters (newlines, tabs, etc.). */
    for (char *p = tmp; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f)
            *p = ' ';
    }

    strncpy(buf, tmp, bufsz - 1);
    buf[bufsz - 1] = '\0';
}

/* ---- Halftoning ------------------------------------------------------- */

/* 16×16 blue noise dither matrix, gamma-corrected for laser dot gain.
 *
 * Base: void-and-cluster blue noise (from hp-printer-app / Michael Sweet).
 * Correction formula: threshold = 255 - 255*(1 - raw/255)^0.55
 *   Exponent 0.55 is tuned for this printer: slightly above the pure sRGB
 *   inverse gamma (0.4545) to account for the HL-5170DN's dot gain on
 *   standard copy paper.  A 50% gray input prints ~28% of dots, which
 *   fuse/spread to give perceptually 50% coverage.
 *   (To adjust: increase exponent → darker; decrease → lighter.)
 *
 * Convention: pixel < threshold → print black dot  (0=black, 255=white)
 * Applied at all resolutions (300 and 600 dpi).
 *
 * Coverage curve for reference:
 *   V=  0 (black): ~100% dots   V=128 (50% gray): ~28% dots
 *   V= 64 (dark):  ~59% dots    V=192 (light):     ~8% dots
 *   V=255 (white):   0% dots
 */
static const unsigned char dither16[16][16] = {
    {  69,  28,  92, 109,  70, 140,  42, 122, 146,  29,  99,  57,  39,  21,  51, 233 },
    {  14,  60, 199, 172,  18, 226,  97,  11,  22,  65, 169, 116, 139,  89,   7, 113 },
    {  79, 123,  47,   8,  38, 118,  77,  52, 160,  83, 217,  13,  72,  31, 182, 159 },
    {  23, 148, 100,  84, 134,  64,  31, 194, 108,  36,   1, 126,  46, 203,  96,  40 },
    {   1, 210,  33,  55, 184,   3, 150,  16, 132,  62,  94, 152,  19,  56, 135,  66 },
    { 174, 110,  71,  20, 162, 103,  90,  41, 212,  50, 178,  28,  80, 105,   9,  87 },
    {  50, 141,  12, 243,  45,  26, 124,  72,   7,  23, 115,  65, 237, 121, 158,  34 },
    {  61, 125,  94,  77, 117,  59, 192,  82, 163,  98, 144,   4,  43,  15, 197,  25 },
    { 187,  18,  41,   6, 151,  34,  10, 138,  53,  35,  69, 171,  90,  52,  75, 101 },
    { 154,  83, 208, 107, 175,  68,  19, 220, 111,  13, 190, 129,  30, 143, 117,   3 },
    {  67, 133,  29,  54,  88, 131, 102,  46,  27,  86,  60, 104,  20, 223,  58,  37 },
    {   9,  44, 168,  22,   0,  39, 181,  76, 142, 201,   2,  44,  80,  11, 179,  93 },
    { 214, 120,  75, 145, 229,  63,  95,   8, 156, 119,  67, 167, 137,  49, 149, 109 },
    {  16,  56,  98,  12, 112, 127,  32,  17,  54,  37,  24,  91, 114,  33,  73,  26 },
    { 164, 189,  36,  81,  48, 196, 165,  74, 106, 255, 130,  15, 205,  62,   2,  85 },
    {  43, 136,   5, 157,  25,  58,   4,  87, 185,  48,   6,  78, 176, 153, 103, 128 },
};

/* Blue noise ordered dither: 8-bit grayscale → 1-bit packed (1=black).
 * Used at all resolutions; the gamma correction baked into dither16 makes
 * the 300 dpi path look as good as the old 600 dpi clustered-dot path. */
static void dither_row_bn(const unsigned char *src, unsigned char *dst,
                          unsigned w, unsigned y)
{
    unsigned row = y & 15u;
    memset(dst, 0, (w + 7u) / 8u);
    for (unsigned x = 0; x < w; x++) {
        if (src[x] < dither16[row][x & 15u])
            dst[x >> 3] |= (unsigned char)(0x80u >> (x & 7u));
    }
}

/* ---- IPP → PJL mapping helpers --------------------------------------- */

static const char *size_name_to_pjl(const char *name)
{
    if (!strcmp(name, "na_letter_8.5x11in"))          return "LETTER";
    if (!strcmp(name, "na_legal_8.5x14in"))            return "LEGAL";
    if (!strcmp(name, "na_executive_7.25x10.5in"))     return "EXECUTIVE";
    if (!strcmp(name, "iso_a4_210x297mm"))              return "A4";
    if (!strcmp(name, "iso_a5_148x210mm"))              return "A5";
    if (!strcmp(name, "iso_a6_105x148mm"))              return "A6";
    if (!strcmp(name, "na_number-10_4.125x9.5in"))     return "COM10";
    if (!strcmp(name, "na_monarch_3.875x7.5in"))        return "MONARCH";
    if (!strcmp(name, "iso_dl_110x220mm"))              return "DL";
    if (!strcmp(name, "iso_c5_162x229mm"))              return "C5";
    if (!strcmp(name, "iso_b5_176x250mm"))              return "B5";
    return "LETTER";
}

/* Imageable area in PostScript points (72 dpi basis) for each supported
 * media size.  Values from legacy/Brother-HL5170DN-PCL.ppd ImageableArea
 * entries: all sizes have 18pt side margins and 12pt top/bottom margins.
 * Used only by the disabled APT path (apt_render_pdf). */
static void paper_imageable_pts(const char *name, int *w_pts, int *h_pts)
{
    static const struct { const char *n; int w, h; } t[] = {
        { "na_letter_8.5x11in",           576, 768 },
        { "na_legal_8.5x14in",            576, 984 },
        { "na_executive_7.25x10.5in",     486, 732 },
        { "iso_a4_210x297mm",             559, 818 },
        { "iso_a5_148x210mm",             385, 571 },
        { "iso_a6_105x148mm",             261, 396 },
        { "na_number-10_4.125x9.5in",     261, 660 },
        { "na_monarch_3.875x7.5in",       243, 516 },
        { "iso_dl_110x220mm",             276, 600 },
        { "iso_c5_162x229mm",             423, 625 },
        { "iso_b5_176x250mm",             445, 685 },
    };
    for (size_t i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        if (!strcmp(name, t[i].n)) { *w_pts = t[i].w; *h_pts = t[i].h; return; }
    }
    *w_pts = 576; *h_pts = 768;  /* fallback: letter */
}

/* Full media dimensions in PostScript points for the GS raster pipeline.
 * GS renders the PDF 1:1 at these dimensions (no scaling); the printer's
 * hardware clips the physical unprintable margins naturally.  Using the
 * full page size (not the imageable area) avoids the aspect-ratio mismatch
 * that -dFitPage would introduce: with imageable area dimensions, -dFitPage
 * scales content down ~6% and pads ~8 mm of blank space at the top of the
 * raster, shifting all content downward. */
static void paper_size_pts(const char *name, int *w_pts, int *h_pts)
{
    static const struct { const char *n; int w, h; } t[] = {
        { "na_letter_8.5x11in",           612, 792 },
        { "na_legal_8.5x14in",            612, 1008 },
        { "na_executive_7.25x10.5in",     522, 756 },
        { "iso_a4_210x297mm",             595, 842 },
        { "iso_a5_148x210mm",             420, 595 },
        { "iso_a6_105x148mm",             297, 420 },
        { "na_number-10_4.125x9.5in",     297, 684 },
        { "na_monarch_3.875x7.5in",       279, 540 },
        { "iso_dl_110x220mm",             312, 624 },
        { "iso_c5_162x229mm",             459, 649 },
        { "iso_b5_176x250mm",             499, 709 },
    };
    for (size_t i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        if (!strcmp(name, t[i].n)) { *w_pts = t[i].w; *h_pts = t[i].h; return; }
    }
    *w_pts = 612; *h_pts = 792;  /* fallback: letter */
}

/* ---- Phase 3: media substitution helpers ------------------------------ */

static bool is_envelope(const char *name)
{
    return !strncmp(name, "na_number-10", 12) ||
           !strncmp(name, "na_monarch",   10) ||
           !strncmp(name, "iso_dl",        6) ||
           !strncmp(name, "iso_c5",        6) ||
           !strncmp(name, "iso_b5",        6);
}

/* Returns the coerced PWG size name, or NULL if no substitution is needed. */
static const char *coerce_size(const char *requested, const char *loaded)
{
    if (is_envelope(requested) || !strcmp(requested, loaded))
        return NULL;

    if (!strcmp(loaded, "na_letter_8.5x11in")) {
        if (!strcmp(requested, "iso_a4_210x297mm")        ||
            !strcmp(requested, "iso_a5_148x210mm")        ||
            !strcmp(requested, "iso_a6_105x148mm")        ||
            !strcmp(requested, "na_legal_8.5x14in")       ||
            !strcmp(requested, "na_executive_7.25x10.5in"))
            return "na_letter_8.5x11in";
    } else if (!strcmp(loaded, "iso_a4_210x297mm")) {
        if (!strcmp(requested, "na_letter_8.5x11in")      ||
            !strcmp(requested, "na_legal_8.5x14in")       ||
            !strcmp(requested, "na_executive_7.25x10.5in") ||
            !strcmp(requested, "iso_a5_148x210mm")        ||
            !strcmp(requested, "iso_a6_105x148mm"))
            return "iso_a4_210x297mm";
    }
    return NULL;
}

/* Dimensions in 100ths of mm for the two supported loaded-paper sizes. */
static void loaded_paper_dims(const char *size_name, int *w, int *l)
{
    if (!strcmp(size_name, "iso_a4_210x297mm"))
        { *w = 21000; *l = 29700; }
    else
        { *w = 21590; *l = 27940; }  /* Letter — default */
}

/* Apply media substitution to options->media in place.
 * Returns false (and rejects the job) when mismatch-action=reject.
 * On substitute, rewrites options->media.size_name + dimensions and sets
 * job notifications through all three channels (log, message, reasons). */
static bool apply_media_substitution(pappl_job_t *job, pappl_pr_options_t *options,
                                     const char *ctx)
{
    const char *loaded = cupsGetOption("loaded-paper",
                             options->num_vendor, options->vendor);
    if (!loaded || !*loaded)
        loaded = "na_letter_8.5x11in";

    const char *action = cupsGetOption("media-mismatch-action",
                             options->num_vendor, options->vendor);
    if (!action || !*action)
        action = "substitute";

    const char *coerced = coerce_size(options->media.size_name, loaded);
    if (!coerced)
        return true;  /* no substitution needed */

    const char *orig_pjl   = size_name_to_pjl(options->media.size_name);
    const char *loaded_pjl = size_name_to_pjl(loaded);

    if (!strcmp(action, "reject")) {
        papplLogJob(job, PAPPL_LOGLEVEL_WARN,
            "%s: rejecting job: loaded paper is %s, requested %s",
            ctx, loaded_pjl, orig_pjl);
        papplJobSetMessage(job,
            "Loaded paper is %s; requested %s. "
            "Change client setting or reload printer.",
            loaded_pjl, orig_pjl);
        papplJobSetReasons(job, PAPPL_JREASON_DOCUMENT_UNPRINTABLE_ERROR,
            PAPPL_JREASON_NONE);
        return false;
    }

    /* substitute mode: rewrite media to the loaded paper */
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
        "%s: substituted %s for %s (loaded paper)", ctx, loaded_pjl, orig_pjl);
    papplJobSetMessage(job, "Substituted %s for requested %s",
        loaded_pjl, orig_pjl);
    papplJobSetReasons(job, PAPPL_JREASON_WARNINGS_DETECTED, PAPPL_JREASON_NONE);

    strncpy(options->media.size_name, coerced,
            sizeof(options->media.size_name) - 1);
    options->media.size_name[sizeof(options->media.size_name) - 1] = '\0';
    loaded_paper_dims(coerced, &options->media.size_width,
                      &options->media.size_length);
    return true;
}

static const char *source_to_pjl(const char *source)
{
    if (!strcmp(source, "tray-1"))       return "TRAY1";
    if (!strcmp(source, "by-pass-tray")) return "MP";
    return "AUTO";
}

static const char *type_to_pjl(const char *type)
{
    if (!strcmp(type, "stationery"))             return "REGULAR";
    if (!strcmp(type, "stationery-lightweight")) return "THIN";
    if (!strcmp(type, "cardstock"))              return "THICK";
    if (!strcmp(type, "cardstock-heavy"))        return "THICK2";
    if (!strcmp(type, "bond"))                   return "BOND";
    if (!strcmp(type, "envelope"))               return "ENVELOPES";
    if (!strcmp(type, "envelope-heavy"))         return "ENVTHICK";
    if (!strcmp(type, "envelope-lightweight"))   return "ENVTHIN";
    return "REGULAR";
}

/* Build PJL params from PAPPL print options.  Called in rstartjob_cb. */
static void pjl_params_from_options(const pappl_pr_options_t *opts,
                                    pjl_job_params_t *p)
{
    bool is_duplex = (opts->sides == PAPPL_SIDES_TWO_SIDED_LONG_EDGE ||
                      opts->sides == PAPPL_SIDES_TWO_SIDED_SHORT_EDGE);

    p->resolution    = (int)opts->header.HWResolution[0];
    p->powersave_off = true;
    p->duplex        = is_duplex;
    p->binding       = (opts->sides == PAPPL_SIDES_TWO_SIDED_SHORT_EDGE)
                       ? "SHORTEDGE" : "LONGEDGE";
    p->paper         = size_name_to_pjl(opts->media.size_name);
    p->source        = source_to_pjl(opts->media.source);
    p->mediatype     = type_to_pjl(opts->media.type);
    p->economode     = (opts->print_quality == IPP_QUALITY_DRAFT);
    p->copies        = opts->copies > 0 ? opts->copies : 1;

    /* Draft quality forces 300 dpi + economode regardless of resolution attr. */
    if (p->economode && p->resolution > 300)
        p->resolution = 300;

    /* APT disabled — see reference code block above.
     * APT_DISABLED: restore the original line to re-enable:
     *   p->apt = (opts->print_quality == IPP_QUALITY_HIGH); */
    p->apt = false;

    /* High quality (Best on iOS) forces 600 dpi even without APT.
     * This gives iOS users a meaningful three-way choice:
     *   Draft (3) = 300 dpi + economode   — fast, toner-saving
     *   Normal(4) = 300 dpi, no economode — standard output
     *   Best  (5) = 600 dpi, no economode — full quality for photos
     * Apps that set printer-resolution directly can still override independently. */
    if (opts->print_quality == IPP_QUALITY_HIGH)
        p->resolution = 600;
}

/* ---- Raster callbacks -------------------------------------------------- */

static bool hl5170dn_rstartjob(pappl_job_t *job, pappl_pr_options_t *options,
                                pappl_device_t *device)
{
    hl5170dn_job_t *jd = calloc(1, sizeof(*jd));
    if (!jd) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "rstartjob: out of memory");
        return false;
    }

    /* Phase 4: build rich log context before the first log call. */
    make_job_ctx(job, jd->ctx, sizeof(jd->ctx));

    /* Phase 3: rewrite options->media before pjl_params_from_options so that
     * both PJL PAPER= and the GS -sPAPERSIZE= (read after this callback in
     * pdf_filter_cb) see the loaded paper, not the client-requested size. */
    if (!apply_media_substitution(job, options, jd->ctx)) {
        free(jd);
        return false;
    }

    pjl_job_params_t pjl;
    pjl_params_from_options(options, &pjl);

    jd->start_time = time(NULL);
    jd->resolution = pjl.resolution;
    /* halftone_buf and line_buf are NULL/0 from calloc; allocated in rstartpage */

    papplJobSetData(job, jd);

    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
        "%s: start job: %ddpi duplex=%s paper=%s source=%s type=%s econo=%s copies=%d",
        jd->ctx,
        pjl.resolution,
        pjl.duplex ? (pjl.binding ? pjl.binding : "ON") : "OFF",
        pjl.paper    ? pjl.paper    : "?",
        pjl.source   ? pjl.source   : "?",
        pjl.mediatype? pjl.mediatype: "?",
        pjl.economode? "ON" : "OFF",
        pjl.copies);

    pjl_write_job_header(device, &pjl);
    return true;
}

static bool hl5170dn_rstartpage(pappl_job_t *job, pappl_pr_options_t *options,
                                 pappl_device_t *device, unsigned page)
{
    hl5170dn_job_t *jd  = papplJobGetData(job);
    unsigned        w   = options->header.cupsWidth;
    char            buf[64];
    int             n;

    if (w == 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
            "rstartpage page=%u: cupsWidth=0", page);
        return false;
    }

    /* Allocate/resize per-line buffers when page width changes. */
    if (w != jd->page_width) {
        size_t row1bit    = (w + 7u) / 8u;
        size_t packed_max = packbits_max(row1bit);
        unsigned char *hbuf = malloc(row1bit);
        unsigned char *lbuf = malloc(packed_max);
        if (!hbuf || !lbuf) {
            free(hbuf);
            free(lbuf);
            papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                "rstartpage: OOM allocating %zu + %zu bytes", row1bit, packed_max);
            return false;
        }
        free(jd->halftone_buf);
        free(jd->line_buf);
        jd->halftone_buf  = hbuf;
        jd->line_buf      = lbuf;
        jd->line_buf_size = packed_max;
        jd->page_width    = w;
    }

    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
        "%s: start page %u: %ux%u px at %ddpi",
        jd->ctx, page, w, options->header.cupsHeight, jd->resolution);

    /* PCL raster setup (page 0 only: duplex mode):
     *   ESC &l<N>S     — PCL duplex mode: 0=simplex, 1=long-edge, 2=short-edge
     *                    Sent first, before any other PCL, so it takes effect on
     *                    page 1. ESC E before this command defers it to page 2.
     *   ESC *t<N>R     — raster resolution N dpi
     *   ESC *r0F       — presentation: portrait, no rotation
     *   ESC *b2M       — compression: TIFF packbits (mode 2)
     *   ESC *r1A       — start raster at top-left of page */
    if (page == 0) {
        if (options->sides == PAPPL_SIDES_TWO_SIDED_SHORT_EDGE)
            papplDeviceWrite(device, "\x1b&l2S", 5);
        else if (options->sides == PAPPL_SIDES_TWO_SIDED_LONG_EDGE)
            papplDeviceWrite(device, "\x1b&l1S", 5);
        else
            papplDeviceWrite(device, "\x1b&l0S", 5);  /* explicit simplex */
    }
    n = snprintf(buf, sizeof(buf),
        "\x1b&a0L"    /* reset left margin to col 0 (left of printable area) */
        "\x1b*t%dR"   /* raster resolution */
        "\x1b*r0F"    /* presentation: portrait, no rotation */
        "\x1b*b2M"    /* compression: packbits */
        "\x1b*r0A",   /* start raster at left margin (=0), top of raster area */
        jd->resolution);
    papplDeviceWrite(device, buf, (size_t)n);
    papplDeviceFlush(device);
    return true;
}

static bool hl5170dn_rwriteline(pappl_job_t *job, pappl_pr_options_t *options,
                                 pappl_device_t *device, unsigned y,
                                 const unsigned char *line)
{
    hl5170dn_job_t *jd  = papplJobGetData(job);
    unsigned        w   = options->header.cupsWidth;
    char            hdr[32];
    int             hdr_len;
    size_t          encoded_len;

    /* Periodic trace: y=0 and every 256 lines thereafter. */
    if ((y % 256) == 0)
        papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
            "%s: rwriteline y=%u width=%u line[0]=0x%02x",
            jd->ctx, y, w, line[0]);

    /* Halftone 8-bit grayscale → 1-bit packed row in halftone_buf.
     * Blue noise dither with gamma correction at all resolutions. */
    dither_row_bn(line, jd->halftone_buf, w, y);

    encoded_len = packbits_encode(jd->halftone_buf, (w + 7u) / 8u, jd->line_buf);

    /* PCL transfer raster data: ESC *b <count> W <data> */
    hdr_len = snprintf(hdr, sizeof(hdr), "\x1b*b%uW", (unsigned)encoded_len);
    papplDeviceWrite(device, hdr, (size_t)hdr_len);
    papplDeviceWrite(device, jd->line_buf, encoded_len);

    return true;
}

static bool hl5170dn_rendpage(pappl_job_t *job, pappl_pr_options_t *options,
                               pappl_device_t *device, unsigned page)
{
    static const char end_page[] =
        "\x1b*rC"   /* ESC *r C — end raster transfer */
        "\x0c";     /* form feed — advance page (duplex: hold and flip) */

    (void)options;

    hl5170dn_job_t *jd2 = papplJobGetData(job);
    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "%s: end page %u",
        jd2 ? jd2->ctx : "?", page);
    papplDeviceWrite(device, end_page, sizeof(end_page) - 1);
    papplDeviceFlush(device);
    return true;
}

static bool hl5170dn_rendjob(pappl_job_t *job, pappl_pr_options_t *options,
                              pappl_device_t *device)
{
    hl5170dn_job_t *jd = papplJobGetData(job);
    (void)options;

    if (!jd)
        return true;   /* rstartjob_cb failed before setting job data */

    papplLogJob(job, PAPPL_LOGLEVEL_INFO, "%s: end job (elapsed %lds)",
        jd->ctx, (long)(time(NULL) - jd->start_time));

    pjl_write_job_trailer(device, /*restore_powersave=*/true);

    free(jd->halftone_buf);
    free(jd->line_buf);
    free(jd);
    papplJobSetData(job, NULL);
    return true;
}

/* ---- Identify / status stubs ------------------------------------------ */

static void hl5170dn_identify(pappl_printer_t *printer,
                               pappl_identify_actions_t actions,
                               const char *message)
{
    (void)printer;
    (void)actions;
    (void)message;
    /* HL-5170DN has no audible/visual identify mechanism. */
}

/* ---- Supply baseline helpers ------------------------------------------ */

#define TONER_RATED_PAGES  6700
#define DRUM_RATED_PAGES   25000
#define SUPPLY_CONF_DIR    "/var/lib/hl5170dn-printer-app"
#define SUPPLY_CONF_PATH   "/var/lib/hl5170dn-printer-app/supply-baselines.conf"

typedef struct {
    long toner_baseline;
    long drum_baseline;
    long last_page_count;    /* -1 = never polled */
    long toner_reset_time;   /* unix timestamp, 0 = never recorded */
    long drum_reset_time;    /* unix timestamp, 0 = never recorded */
} supply_baselines_t;

static void read_baselines(supply_baselines_t *b)
{
    b->toner_baseline  = 0;
    b->drum_baseline   = 0;
    b->last_page_count = -1;
    b->toner_reset_time = 0;
    b->drum_reset_time  = 0;

    FILE *f = fopen(SUPPLY_CONF_PATH, "r");
    if (!f)
        return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = line;
        long  val = atol(eq + 1);
        if (strcmp(key, "toner_baseline") == 0)
            b->toner_baseline = val;
        else if (strcmp(key, "drum_baseline") == 0)
            b->drum_baseline = val;
        else if (strcmp(key, "last_page_count") == 0)
            b->last_page_count = val;
        else if (strcmp(key, "toner_reset_time") == 0)
            b->toner_reset_time = val;
        else if (strcmp(key, "drum_reset_time") == 0)
            b->drum_reset_time = val;
    }
    fclose(f);
}

static void write_conf(const supply_baselines_t *b)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", SUPPLY_CONF_PATH);

    mkdir(SUPPLY_CONF_DIR, 0755);

    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f,
            "toner_baseline=%ld\ndrum_baseline=%ld\nlast_page_count=%ld\n"
            "toner_reset_time=%ld\ndrum_reset_time=%ld\n",
            b->toner_baseline, b->drum_baseline, b->last_page_count,
            b->toner_reset_time, b->drum_reset_time);
    fclose(f);
    rename(tmp, SUPPLY_CONF_PATH);
}

static int supply_level(long total_pages, long baseline, long rated)
{
    long used = total_pages - baseline;
    if (used < 0)
        used = 0;
    long level = 100 - (used * 100 / rated);
    if (level < 0)   level = 0;
    if (level > 100) level = 100;
    return (int)level;
}

static bool hl5170dn_status(pappl_printer_t *printer)
{
    /* Open USB device for PJL back-channel query.
     * Returns NULL if device is busy (job in progress) — that's fine. */
    pappl_device_t *device = papplPrinterOpenDevice(printer);
    if (!device) {
        papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
            "status: device busy, skipping PJL poll");
        return true;
    }

    /* Send @PJL INFO STATUS and @PJL INFO PAGECOUNT in one session. */
    static const char query[] =
        "\033%-12345X@PJL\r\n"
        "@PJL INFO STATUS\r\n"
        "@PJL INFO PAGECOUNT\r\n"
        "\033%-12345X";
    papplDeviceWrite(device, query, sizeof(query) - 1);
    papplDeviceFlush(device);

    /* Read response.  The printer may return INFO STATUS and INFO PAGECOUNT
     * in one packet or split across two.  Do a second read if PAGECOUNT is
     * missing from the first. */
    char    buf[2048];
    ssize_t bytes = papplDeviceRead(device, buf, sizeof(buf) - 1);

    if (bytes <= 0) {
        papplPrinterCloseDevice(printer);
        papplLogPrinter(printer, PAPPL_LOGLEVEL_WARN,
            "status: no response from printer (bytes=%ld)", (long)bytes);
        return true;
    }
    buf[bytes] = '\0';

    /* If PAGECOUNT not yet in buffer, try one more read for the second packet. */
    if (!strstr(buf, "PAGECOUNT=")) {
        char    buf2[1024];
        ssize_t bytes2 = papplDeviceRead(device, buf2, sizeof(buf2) - 1);
        if (bytes2 > 0) {
            buf2[bytes2] = '\0';
            /* Append to buf if space allows, otherwise just search buf2. */
            if ((size_t)(bytes + bytes2) < sizeof(buf) - 1) {
                memcpy(buf + bytes, buf2, (size_t)bytes2 + 1);
                bytes += bytes2;
            } else {
                /* buf2 is separate; copy PAGECOUNT= value if found. */
                char *pc = strstr(buf2, "PAGECOUNT=");
                if (pc) {
                    /* Append just the PAGECOUNT= token to buf. */
                    size_t avail = sizeof(buf) - 1 - (size_t)bytes;
                    size_t tlen  = strlen(pc);
                    if (tlen < avail) {
                        memcpy(buf + bytes, pc, tlen + 1);
                        bytes += (ssize_t)tlen;
                    }
                }
            }
        }
    }

    papplPrinterCloseDevice(printer);

    /* Parse CODE=, ONLINE=, and PAGECOUNT= from the (possibly combined) buffer. */
    int  code       = -1;
    bool online     = false;
    long page_count = -1;
    char *p;
    if ((p = strstr(buf, "CODE=")) != NULL)
        code = atoi(p + 5);
    if ((p = strstr(buf, "ONLINE=")) != NULL)
        online = (strncmp(p + 7, "TRUE", 4) == 0);
    if ((p = strstr(buf, "PAGECOUNT=")) != NULL)
        page_count = atol(p + 10);

    papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
        "status: CODE=%d ONLINE=%s PAGECOUNT=%ld",
        code, online ? "TRUE" : "FALSE", page_count);

    /* Map status code range to printer reasons.
     * 40000–40999: operator-attention errors (paper empty, cover open, jam).
     * All other codes (10xxx ready, 40000=sleep per Phase 0) are non-error. */
    if (code >= 40001 && code <= 40999)
        papplPrinterSetReasons(printer, PAPPL_PREASON_OTHER, PAPPL_PREASON_NONE);
    else
        papplPrinterSetReasons(printer, PAPPL_PREASON_NONE, PAPPL_PREASON_OTHER);

    /* Compute and set supply levels only when we have a real page count.
     * If page_count is still -1 (e.g. printer asleep and second read also
     * returned nothing), leave PAPPL's existing supply values untouched so
     * a previously-recorded good reading is not overwritten with unknowns. */
    if (page_count >= 0) {
        supply_baselines_t baselines;
        read_baselines(&baselines);
        baselines.last_page_count = page_count;
        write_conf(&baselines);

        pappl_supply_t supplies[2];
        memset(supplies, 0, sizeof(supplies));

        /* Toner cartridge (TN-570, 6700-page rated life). */
        supplies[0].color       = PAPPL_SUPPLY_COLOR_BLACK;
        supplies[0].is_consumed = true;
        supplies[0].type        = PAPPL_SUPPLY_TYPE_TONER_CARTRIDGE;
        strncpy(supplies[0].description, "Black Toner Cartridge",
                sizeof(supplies[0].description) - 1);
        supplies[0].level = supply_level(page_count, baselines.toner_baseline,
                                         TONER_RATED_PAGES);

        /* Drum unit (DR-580, 25000-page rated life). */
        supplies[1].color       = PAPPL_SUPPLY_COLOR_BLACK;
        supplies[1].is_consumed = true;
        supplies[1].type        = PAPPL_SUPPLY_TYPE_OPC;
        strncpy(supplies[1].description, "Drum Unit",
                sizeof(supplies[1].description) - 1);
        supplies[1].level = supply_level(page_count, baselines.drum_baseline,
                                         DRUM_RATED_PAGES);

        papplPrinterSetSupplies(printer, 2, supplies);
    }

    return true;
}

/* ---- Supplies web page (replaces PAPPL built-in) ----------------------- */

/* Format a unix timestamp as "YYYY-MM-DD", or "unknown" if ts==0. */
static void fmt_date(long ts, char *buf, size_t bufsz)
{
    if (ts == 0) {
        snprintf(buf, bufsz, "unknown");
        return;
    }
    time_t t = (time_t)ts;
    struct tm *tm = localtime(&t);
    strftime(buf, bufsz, "%Y-%m-%d", tm);
}

/* Render one supply block: bar meter, stats row, reset button. */
static void render_supply(pappl_client_t *client,
                          const char     *supplies_path,
                          const char     *name,
                          int             pct,
                          long            baseline,
                          long            reset_time,
                          long            last_page,
                          long            rated,
                          const char     *reset_action)
{
    long pages_used      = (last_page >= baseline) ? last_page - baseline : 0;
    long pages_remaining = rated - pages_used;
    if (pages_remaining < 0) pages_remaining = 0;
    long expected_end    = baseline + rated;

    char reset_date[32];
    fmt_date(reset_time, reset_date, sizeof(reset_date));

    /* Bar meter (reuses PAPPL's .meter CSS). */
    double filled = pct * 0.5;
    double empty  = 50.0 - filled;
    papplClientHTMLPrintf(client,
        "          <h3>%s &mdash; %d%%</h3>\n"
        "          <table class=\"meter\" summary=\"%s\">\n"
        "            <thead><tr><th></th><td></td><td></td>"
        "<td></td><td></td></tr></thead>\n"
        "            <tbody><tr><th>%d%%</th>"
        "<td colspan=\"4\">"
        "<span class=\"bar\" style=\"background:#222;"
        "padding:0px %.1f%%;\" title=\"%d%%\"></span>"
        "<span class=\"bar\" style=\"background:transparent;"
        "padding:0px %.1f%%;\" title=\"%d%%\"></span>"
        "</td></tr></tbody>\n"
        "            <tfoot><tr><th></th><td></td><td></td>"
        "<td></td><td></td></tr></tfoot>\n"
        "          </table>\n",
        name, pct, name,
        pct,
        filled, pct,
        empty,  pct);

    /* Stats table. */
    if (last_page < 0) {
        papplClientHTMLPuts(client,
            "          <p><em>Page count unavailable &mdash; "
            "printer may be asleep.</em></p>\n");
    } else {
        const char *replaced_label =
            (reset_time == 0 && baseline == 0) ? "Installed at" : "Replaced at";
        papplClientHTMLPrintf(client,
            "          <table>\n"
            "            <tbody>\n"
            "              <tr><th>%s</th>"
            "<td>Page %ld &middot; %s</td></tr>\n"
            "              <tr><th>Pages used</th>"
            "<td>%ld of %ld rated (%d%%)</td></tr>\n"
            "              <tr><th>Est. remaining</th>"
            "<td>~%ld pages &middot; expected end at lifetime page ~%ld</td>"
            "</tr>\n"
            "            </tbody>\n"
            "          </table>\n",
            replaced_label,
            baseline, reset_date,
            pages_used, rated, pct,
            pages_remaining, expected_end);
    }

    /* Reset button. */
    papplClientHTMLStartForm(client, supplies_path, false);
    papplClientHTMLPrintf(client,
        "<input type=\"hidden\" name=\"action\" value=\"%s\">"
        "<button class=\"btn\" type=\"submit\">Reset %s</button>"
        "</form>\n",
        reset_action, name);
}

static bool hl5170dn_web_supplies(pappl_client_t  *client,
                                  pappl_printer_t *printer)
{
    supply_baselines_t conf;
    read_baselines(&conf);

    /* Handle POST (reset action). */
    if (papplClientGetMethod(client) == HTTP_STATE_POST)
    {
        int            num_form = 0;
        cups_option_t *form     = NULL;
        const char    *action;

        num_form = papplClientGetForm(client, &form);
        if (num_form > 0 && papplClientIsValidForm(client, num_form, form) &&
            (action = cupsGetOption("action", num_form, form)) != NULL &&
            conf.last_page_count >= 0)
        {
            if (!strcmp(action, "reset-toner")) {
                conf.toner_baseline   = conf.last_page_count;
                conf.toner_reset_time = (long)time(NULL);
                write_conf(&conf);
            } else if (!strcmp(action, "reset-drum")) {
                conf.drum_baseline   = conf.last_page_count;
                conf.drum_reset_time = (long)time(NULL);
                write_conf(&conf);
            }
        }
        cupsFreeOptions(num_form, form);

        /* Always redirect back to GET to avoid re-POST on refresh. */
        char path[256];
        papplPrinterGetPath(printer, "supplies", path, sizeof(path));
        papplClientRespondRedirect(client, HTTP_STATUS_FOUND, path);
        return true;
    }

    /* GET: render the supplies page. */
    int toner_pct = 0, drum_pct = 0;
    if (conf.last_page_count >= 0) {
        toner_pct = supply_level(conf.last_page_count, conf.toner_baseline,
                                 TONER_RATED_PAGES);
        drum_pct  = supply_level(conf.last_page_count, conf.drum_baseline,
                                 DRUM_RATED_PAGES);
    }

    papplClientHTMLPrinterHeader(client, printer, "Supplies", 0, NULL, NULL);

    papplClientHTMLPuts(client, "          <div class=\"section\">\n");

    if (conf.last_page_count >= 0)
        papplClientHTMLPrintf(client,
            "          <p>Printer lifetime page count: "
            "<strong>%ld</strong></p>\n",
            conf.last_page_count);

    char supplies_path[256];
    papplPrinterGetPath(printer, "supplies", supplies_path,
                        sizeof(supplies_path));

    render_supply(client, supplies_path,
                  "Black Toner Cartridge",
                  toner_pct,
                  conf.toner_baseline, conf.toner_reset_time,
                  conf.last_page_count,
                  TONER_RATED_PAGES,
                  "reset-toner");

    render_supply(client, supplies_path,
                  "Drum Unit",
                  drum_pct,
                  conf.drum_baseline, conf.drum_reset_time,
                  conf.last_page_count,
                  DRUM_RATED_PAGES,
                  "reset-drum");

    papplClientHTMLPuts(client,
        "          <p><small>Click Reset after replacing a consumable to "
        "record the current page count as the new baseline.</small></p>\n"
        "          </div>\n");

    papplClientHTMLPrinterFooter(client);
    return true;
}

void register_supply_reset_route(pappl_system_t  *system,
                                 pappl_printer_t *printer)
{
    char path[256];
    papplPrinterGetPath(printer, "supplies", path, sizeof(path));
    papplSystemAddResourceCallback(system, path, "text/html",
        (pappl_resource_cb_t)hl5170dn_web_supplies, printer);
    papplPrinterAddLink(printer, "Supplies", path,
        PAPPL_LOPTIONS_NAVIGATION | PAPPL_LOPTIONS_STATUS);
}

/* ---- Driver registration ---------------------------------------------- */

/* Helper: fill a pappl_media_col_t for a standard paper size. */
static void fill_media(pappl_media_col_t *m, const char *size_name,
                       int width_100mm, int length_100mm, const char *source)
{
    memset(m, 0, sizeof(*m));
    strncpy(m->size_name, size_name, sizeof(m->size_name) - 1);
    m->size_width    = width_100mm;
    m->size_length   = length_100mm;
    m->left_margin   = 500;
    m->right_margin  = 500;
    m->top_margin    = 500;
    m->bottom_margin = 500;
    strncpy(m->source, source, sizeof(m->source) - 1);
    strncpy(m->type, "stationery", sizeof(m->type) - 1);
}

bool driver_cb(pappl_system_t *system, const char *driver_name,
               const char *device_uri, const char *device_id,
               pappl_pr_driver_data_t *data, ipp_t **attrs, void *cbdata)
{
    (void)system;
    (void)device_uri;
    (void)device_id;
    (void)attrs;
    (void)cbdata;

    if (strcmp(driver_name, "hl5170dn") != 0)
        return false;

    /* Do NOT memset(data, 0).  PAPPL pre-initialises dither matrices
     * in data->gdither / data->pdither before calling driver_cb.
     * Zeroing them causes blank pages for grayscale content.
     * Only set the fields this driver actually needs. */

    strncpy(data->make_and_model, "Brother HL-5170DN",
            sizeof(data->make_and_model) - 1);
    data->ppm = 21;

    /* Callbacks */
    data->identify_cb   = hl5170dn_identify;
    data->status_cb     = hl5170dn_status;
    data->rstartjob_cb  = hl5170dn_rstartjob;
    data->rstartpage_cb = hl5170dn_rstartpage;
    data->rwriteline_cb = hl5170dn_rwriteline;
    data->rendpage_cb   = hl5170dn_rendpage;
    data->rendjob_cb    = hl5170dn_rendjob;

    /* Resolutions: 300 (default) and 600 dpi.
     * HQ1200 deferred to Phase 6 (see plan.md §6B). */
    data->num_resolution  = 2;
    data->x_resolution[0] = 300; data->y_resolution[0] = 300;
    data->x_resolution[1] = 600; data->y_resolution[1] = 600;
    data->x_default       = 300; data->y_default       = 300;

    /* Driver owns halftoning: request 8-bit grayscale from PAPPL.
     * No force_raster_type — do not override to BLACK_1. */
    data->raster_types = PAPPL_PWG_RASTER_TYPE_SGRAY_8;

    /* Monochrome only. */
    data->color_supported = PAPPL_COLOR_MODE_MONOCHROME;
    data->color_default   = PAPPL_COLOR_MODE_MONOCHROME;

    data->content_default = PAPPL_CONTENT_AUTO;
    data->scaling_default = PAPPL_SCALING_AUTO;
    data->quality_default = IPP_QUALITY_NORMAL;

    /* Duplex: HL-5170DN has a physical duplex unit. */
    data->duplex = PAPPL_DUPLEX_NORMAL;
    data->sides_supported = PAPPL_SIDES_ONE_SIDED |
                            PAPPL_SIDES_TWO_SIDED_LONG_EDGE |
                            PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
    data->sides_default   = PAPPL_SIDES_TWO_SIDED_LONG_EDGE;
    data->input_face_up   = false;

    /* Identify: declare SOUND but the callback is a no-op. */
    data->identify_default   = PAPPL_IDENTIFY_ACTIONS_SOUND;
    data->identify_supported = PAPPL_IDENTIFY_ACTIONS_SOUND;

    /* Media sizes (IPP PWG names).  PJL names in size_name_to_pjl(). */
    data->num_media = 11;
    data->media[0]  = "na_letter_8.5x11in";
    data->media[1]  = "iso_a4_210x297mm";
    data->media[2]  = "iso_a5_148x210mm";
    data->media[3]  = "iso_a6_105x148mm";
    data->media[4]  = "na_legal_8.5x14in";
    data->media[5]  = "na_executive_7.25x10.5in";
    data->media[6]  = "na_number-10_4.125x9.5in";
    data->media[7]  = "na_monarch_3.875x7.5in";
    data->media[8]  = "iso_dl_110x220mm";
    data->media[9]  = "iso_c5_162x229mm";
    data->media[10] = "iso_b5_176x250mm";

    /* Default: Letter in tray-1. */
    fill_media(&data->media_default, "na_letter_8.5x11in",
               21590, 27940, "tray-1");

    /* Sources */
    data->num_source = 3;
    data->source[0]  = "tray-1";
    data->source[1]  = "by-pass-tray";
    data->source[2]  = "auto";

    /* media_ready: all sources report Letter loaded. */
    fill_media(&data->media_ready[0], "na_letter_8.5x11in", 21590, 27940, "tray-1");
    fill_media(&data->media_ready[1], "na_letter_8.5x11in", 21590, 27940, "by-pass-tray");
    fill_media(&data->media_ready[2], "na_letter_8.5x11in", 21590, 27940, "auto");

    /* Media types (IPP names).  PJL names in type_to_pjl(). */
    data->num_type = 8;
    data->type[0]  = "stationery";
    data->type[1]  = "stationery-lightweight";
    data->type[2]  = "cardstock";
    data->type[3]  = "cardstock-heavy";
    data->type[4]  = "bond";
    data->type[5]  = "envelope";
    data->type[6]  = "envelope-heavy";
    data->type[7]  = "envelope-lightweight";

    /* Output bin */
    data->num_bin    = 1;
    data->bin[0]     = "face-down";
    data->bin_default = 0;

    /* Phase 3 vendor options — exposed in web UI printer settings and
     * settable per-job via lp -o <name>=<value>.
     *   loaded-paper:          na_letter_8.5x11in (default) | iso_a4_210x297mm
     *   media-mismatch-action: substitute (default) | reject */
    data->num_vendor = 2;
    data->vendor[0]  = "loaded-paper";
    data->vendor[1]  = "media-mismatch-action";

    /* We register our own /supplies route in register_supply_reset_route()
     * after papplPrinterCreate, so don't let PAPPL claim that path. */
    data->has_supplies = false;

    return true;
}

/* ---- APT photo path (Mode 1024) — REFERENCE CODE, DISABLED --------------- *
 *
 * APT (Automatic Photo Tuning) lets the printer halftone 8-bit grayscale
 * data at 600 dpi equivalent from a low-resolution (≤150 dpi) input.
 * The entire page travels as one TIFF file inside a single ESC*b<N>W command.
 *
 * Manual reference: Tech_Manual_Ch2_PCL §6.3.8.
 * APT activates when: BitsPerSample=8, Compression=1 (no compression),
 * and the printer operates at 600 dpi.
 *
 * WHY DISABLED (tested 2026-05-14, job #13 pi-printer-apt-image):
 *   Testing on photo and chart/text content showed APT offers no quality
 *   advantage over the standard 600 dpi GS path, and is ~3.5× slower
 *   (43 s vs 12 s for a 4032×3024 photo on Pi 3B+).  The printer-side
 *   halftoning from a 150 dpi TIFF input is outperformed by GS halftoning
 *   directly at 600 dpi.
 *
 *   To re-enable for further experimentation, restore the two lines marked
 *   APT_DISABLED below (one in pjl_params_from_options, one in pdf_filter_cb).
 */

#define APT_TIFF_HDR_SIZE 174u   /* fixed header size for our 12-tag layout */
#define APT_INPUT_DPI     150    /* recommended by manual for APT input */

/* Write a uint16 little-endian into buf at *off, advance *off. */
static void pu16(unsigned char *buf, size_t *off, unsigned v)
{
    buf[(*off)++] = (unsigned char)(v & 0xff);
    buf[(*off)++] = (unsigned char)((v >> 8) & 0xff);
}

/* Write a uint32 little-endian into buf at *off, advance *off. */
static void pu32(unsigned char *buf, size_t *off, unsigned long v)
{
    buf[(*off)++] = (unsigned char)(v & 0xff);
    buf[(*off)++] = (unsigned char)((v >> 8) & 0xff);
    buf[(*off)++] = (unsigned char)((v >> 16) & 0xff);
    buf[(*off)++] = (unsigned char)((v >> 24) & 0xff);
}

/* Write a 12-byte TIFF IFD entry. type: 3=SHORT, 4=LONG, 5=RATIONAL. */
static void ptag(unsigned char *buf, size_t *off,
                 unsigned tag, unsigned type, unsigned long count,
                 unsigned long value_or_offset)
{
    pu16(buf, off, tag);
    pu16(buf, off, type);
    pu32(buf, off, count);
    /* SHORT values are left-justified in the 4-byte value field. */
    if (type == 3)
        pu16(buf, off, (unsigned)value_or_offset), pu16(buf, off, 0);
    else
        pu32(buf, off, value_or_offset);
}

/* Build the 174-byte TIFF header for APT Mode 1024.
 * Little-endian, 12 tags, single strip, no compression, 8 bpp, 150 dpi.
 * buf must be ≥ APT_TIFF_HDR_SIZE bytes. */
static void apt_build_tiff_header(unsigned char *buf, unsigned w, unsigned h)
{
    size_t off = 0;

    /* TIFF file header */
    buf[off++] = 0x49; buf[off++] = 0x49;   /* "II" little-endian */
    pu16(buf, &off, 42);                      /* TIFF magic */
    pu32(buf, &off, 8);                       /* IFD offset */

    /* IFD: 12 entries */
    pu16(buf, &off, 12);

    /* Offsets used in RATIONAL entries:
     *   rational_base = 8 (hdr) + 2 (count) + 12*12 (entries) + 4 (next) = 158
     *   pixel data    = 158 + 8 + 8 = 174  */
    const unsigned long rational_base = 158;
    const unsigned long pixel_offset  = APT_TIFF_HDR_SIZE;

    ptag(buf, &off, 256, 3, 1, w);                    /* ImageWidth */
    ptag(buf, &off, 257, 3, 1, h);                    /* ImageLength */
    ptag(buf, &off, 258, 3, 1, 8);                    /* BitsPerSample=8 → APT */
    ptag(buf, &off, 259, 3, 1, 1);                    /* Compression=none */
    ptag(buf, &off, 262, 3, 1, 1);                    /* PhotometricInterp=min-is-black */
    ptag(buf, &off, 273, 4, 1, pixel_offset);         /* StripOffsets */
    ptag(buf, &off, 277, 3, 1, 1);                    /* SamplesPerPixel */
    ptag(buf, &off, 278, 4, 1, h);                    /* RowsPerStrip */
    ptag(buf, &off, 279, 4, 1, (unsigned long)w * h); /* StripByteCounts */
    ptag(buf, &off, 282, 5, 1, rational_base);        /* XResolution → rational */
    ptag(buf, &off, 283, 5, 1, rational_base + 8);    /* YResolution → rational */
    ptag(buf, &off, 296, 3, 1, 2);                    /* ResolutionUnit=inch */

    pu32(buf, &off, 0);   /* next IFD = none */

    /* Rational data: XResolution = 150/1, YResolution = 150/1 */
    pu32(buf, &off, APT_INPUT_DPI); pu32(buf, &off, 1);
    pu32(buf, &off, APT_INPUT_DPI); pu32(buf, &off, 1);

    (void)off;   /* off == APT_TIFF_HDR_SIZE (174) here */
}

/* Render a PDF job using APT Mode 1024 (printer-side halftoning).
 * Called from pdf_filter_cb when print_quality == IPP_QUALITY_HIGH.
 * GS renders at APT_INPUT_DPI; each page is wrapped in a 174-byte TIFF
 * header and sent as a single ESC*b<N>W command. */
static bool apt_render_pdf(pappl_job_t *job, pappl_pr_options_t *options,
                            pappl_device_t *device,
                            pappl_pr_driver_data_t *drv,
                            const char *filename, const char *ctx)
{
    unsigned char  tiff_hdr[APT_TIFF_HDR_SIZE];
    char           gs_cmd[4096];
    char           line[256];
    FILE          *gs      = NULL;
    unsigned char *rowbuf  = NULL;
    unsigned       prev_w  = 0;
    unsigned       pagenum = 0;
    bool           ok      = false;

    int gs_w_pts, gs_h_pts;
    paper_imageable_pts(options->media.size_name, &gs_w_pts, &gs_h_pts);

    snprintf(gs_cmd, sizeof(gs_cmd),
        "gs -dBATCH -dNOPAUSE -dSAFE "
        "-sDEVICE=pgmraw -r%d -dFitPage "
        "-dFIXEDMEDIA -dDEVICEWIDTHPOINTS=%d -dDEVICEHEIGHTPOINTS=%d "
        "-sOutputFile=- '%s'",
        APT_INPUT_DPI, gs_w_pts, gs_h_pts, filename);

    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
        "%s: apt_render: gs cmd: %s", ctx, gs_cmd);

    gs = popen(gs_cmd, "r");
    if (!gs) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
            "%s: apt_render: popen failed: %s", ctx, strerror(errno));
        return false;
    }

    while (fgets(line, sizeof(line), gs)) {
        int  w, h;
        bool page_ok = true;

        if (strncmp(line, "P5", 2) != 0)
            continue;

        /* Stop at next page boundary if job was cancelled. */
        if (papplJobIsCanceled(job))
            goto done;

        /* Skip comment lines */
        do {
            if (!fgets(line, sizeof(line), gs))
                goto done;
        } while (line[0] == '#');

        if (sscanf(line, "%d %d", &w, &h) != 2 || w <= 0 || h <= 0) {
            papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                "%s: apt_render: bad PGM dims page %u", ctx, pagenum);
            break;
        }
        /* maxval line — consume and discard */
        if (!fgets(line, sizeof(line), gs))
            goto done;

        if ((unsigned)w > prev_w) {
            free(rowbuf);
            rowbuf = malloc((size_t)w);
            if (!rowbuf) {
                papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "%s: apt_render: OOM row buf page %u", ctx, pagenum);
                break;
            }
            prev_w = (unsigned)w;
        }

        unsigned long tiff_size = APT_TIFF_HDR_SIZE + (unsigned long)w * h;

        papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
            "%s: apt_render: page %u: %dx%d px, TIFF=%lu B",
            ctx, pagenum, w, h, tiff_size);

        /* PCL framing: duplex mode (page 0 only), then 600 dpi, Mode 1024.
         * ESC &l<N>S must come before ESC E — otherwise it defers to page 2. */
        if (pagenum == 0) {
            if (options->sides == PAPPL_SIDES_TWO_SIDED_SHORT_EDGE)
                papplDeviceWrite(device, "\x1b&l2S", 5);
            else if (options->sides == PAPPL_SIDES_TWO_SIDED_LONG_EDGE)
                papplDeviceWrite(device, "\x1b&l1S", 5);
            else
                papplDeviceWrite(device, "\x1b&l0S", 5);
        }
        char pcl[128];
        int  pcl_len = snprintf(pcl, sizeof(pcl),
            "\x1b*t600R"
            "\x1b*b1024M"
            "\x1b*r1A"
            "\x1b*b%luW",
            tiff_size);
        papplDeviceWrite(device, pcl, (size_t)pcl_len);

        /* 174-byte TIFF header */
        apt_build_tiff_header(tiff_hdr, (unsigned)w, (unsigned)h);
        papplDeviceWrite(device, tiff_hdr, APT_TIFF_HDR_SIZE);

        /* Stream pixel rows directly from GS stdout — no full-page buffer */
        for (int y = 0; y < h && page_ok && !papplJobIsCanceled(job); y++) {
            if (fread(rowbuf, 1, (size_t)w, gs) != (size_t)w) {
                papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "%s: apt_render: short read y=%d page %u: %s",
                    ctx, y, pagenum, strerror(errno));
                page_ok = false;
            } else {
                papplDeviceWrite(device, rowbuf, (size_t)w);
            }
        }

        /* rendpage_cb sends ESC*rC + form feed and flushes — don't duplicate here */

        /* Keep PAPPL job accounting consistent */
        if (!papplJobIsCanceled(job)) {
            options->header.cupsWidth  = (unsigned)w;
            options->header.cupsHeight = (unsigned)h;
            drv->rendpage_cb(job, options, device, pagenum);
        }

        pagenum++;
        if (!page_ok || papplJobIsCanceled(job))
            break;
    }

done:
    ok = (pagenum > 0);
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
        "%s: apt_render: %s — %u page(s)", ctx, ok ? "ok" : "FAILED", pagenum);
    if (papplJobIsCanceled(job))
        papplLogJob(job, PAPPL_LOGLEVEL_INFO,
            "%s: apt_render: cancelled after %u complete page(s)", ctx, pagenum);

    if (gs) {
        int gs_status = pclose(gs);
        if (gs_status != 0)
            papplLogJob(job, PAPPL_LOGLEVEL_WARN,
                "%s: apt_render: gs exited with status %d", ctx, gs_status);
    }
    free(rowbuf);
    return ok;
}

/* ---- PDF → PCL filter (streaming via Ghostscript) ---------------------- *
 *
 * Registered via papplSystemAddMIMEFilter() (requires PAPPL 1.4+).
 * iPhone AirPrint sends application/pdf; this routes it through GS.
 *
 * Streaming design: GS writes pgmraw (8-bit grayscale) to stdout, which
 * we read through a popen() pipe.  PCL bytes reach the printer as soon
 * as GS produces the first raster row — no silent USB gap, no full-page
 * buffer, responsive to job cancel.
 */
static bool pdf_filter_cb(pappl_job_t *job, pappl_device_t *device, void *cbdata)
{
    const char         *filename = papplJobGetFilename(job);
    pappl_printer_t    *printer  = papplJobGetPrinter(job);
    pappl_pr_driver_data_t drv;
    pappl_pr_options_t *options;
    char                gs_cmd[4096];
    char                line[256];
    FILE               *gs        = NULL;
    unsigned char      *rowbuf    = NULL;
    unsigned            prev_w    = 0;
    unsigned            pagenum   = 0;
    bool                ok        = false;
    bool                job_started = false;
    char                ctx[64];

    (void)cbdata;

    /* Phase 4: build ctx for pdf_filter_cb's own log lines (before rstartjob_cb
     * fires and stores ctx in jd).  After rstartjob_cb we use jd->ctx instead. */
    make_job_ctx(job, ctx, sizeof(ctx));

    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
        "%s: pdf_filter: '%s' via gs pgmraw (streaming)", ctx, filename);

    options = papplJobCreatePrintOptions(job, (unsigned)INT_MAX, false);
    if (!options) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
            "%s: pdf_filter: papplJobCreatePrintOptions failed", ctx);
        return false;
    }

    papplPrinterGetDriverData(printer, &drv);

    /* Start the job: sends PJL header (POWERSAVE=OFF, RESOLUTION, etc.).
     * jd->resolution is set here; we use it for the GS command below. */
    if (!drv.rstartjob_cb(job, options, device)) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
            "%s: pdf_filter: rstartjob_cb failed", ctx);
        goto done;
    }
    job_started = true;

    {
        hl5170dn_job_t *jd = papplJobGetData(job);

        /* APT disabled — see reference code block above.
         * APT_DISABLED: restore the original condition to re-enable:
         *   if (options->print_quality == IPP_QUALITY_HIGH) { */
        if (0) {
            papplLogJob(job, PAPPL_LOGLEVEL_INFO,
                "%s: pdf_filter: using APT Mode 1024 (%d dpi input)",
                ctx, APT_INPUT_DPI);
            ok = apt_render_pdf(job, options, device, &drv, filename, ctx);
            goto done;  /* rendjob_cb called at done: via job_started */
        }

        int gs_w_pts, gs_h_pts;
        paper_size_pts(options->media.size_name, &gs_w_pts, &gs_h_pts);
        int res = jd->resolution;

        /* GS renders the PDF as 8-bit grayscale PGM to stdout.
         * Render at full page dimensions (not the imageable area) so that GS
         * maps PS/PDF coordinates 1:1 to raster rows with no scaling.
         * -dFIXEDMEDIA enforces the correct page size regardless of what the
         * PDF specifies.  -dFitPage is intentionally omitted: it would scale
         * the content down ~6% and introduce ~8 mm of blank space at the top
         * of the raster due to the imageable-area aspect-ratio mismatch.
         * ESC*r0A in rstartpage_cb positions the raster at the top of the
         * printer's physical printable area; hardware clips the unprintable
         * margins naturally. */
        snprintf(gs_cmd, sizeof(gs_cmd),
            "gs -dBATCH -dNOPAUSE -dSAFE "
            "-sDEVICE=pgmraw -r%d "
            "-dFIXEDMEDIA -dDEVICEWIDTHPOINTS=%d -dDEVICEHEIGHTPOINTS=%d "
            "-sOutputFile=- '%s'",
            res, gs_w_pts, gs_h_pts, filename);

        papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
            "%s: pdf_filter: gs cmd: %s", ctx, gs_cmd);
    }

    gs = popen(gs_cmd, "r");
    if (!gs) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
            "%s: pdf_filter: popen failed: %s", ctx, strerror(errno));
        goto done;
    }

    /* Each P5 block in the stream is one page of the PDF. */
    while (fgets(line, sizeof(line), gs)) {
        int    w, h;
        bool   page_ok = true;

        if (strncmp(line, "P5", 2) != 0)
            continue;

        /* Stop at next page boundary if job was cancelled. */
        if (papplJobIsCanceled(job))
            goto jobs_done;

        /* Skip optional comment lines after the P5 magic. */
        do {
            if (!fgets(line, sizeof(line), gs))
                goto jobs_done;
        } while (line[0] == '#');

        /* "width height" line */
        if (sscanf(line, "%d %d", &w, &h) != 2 || w <= 0 || h <= 0) {
            papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                "%s: pdf_filter: bad PGM dimensions on page %u", ctx, pagenum);
            break;
        }

        /* maxval line (always "255\n" from GS pgmraw) — consume and discard */
        if (!fgets(line, sizeof(line), gs))
            goto jobs_done;

        /* Update options for this page's geometry.
         * cupsBytesPerLine = w: one byte per pixel for SGRAY_8. */
        options->header.cupsWidth        = (unsigned)w;
        options->header.cupsHeight       = (unsigned)h;
        options->header.cupsBytesPerLine = (unsigned)w;

        papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
            "%s: pdf_filter: page %u: %dx%d px", ctx, pagenum, w, h);

        /* Grow row buffer if this page is wider than previous pages. */
        if ((unsigned)w > prev_w) {
            free(rowbuf);
            rowbuf = malloc((size_t)w);
            if (!rowbuf) {
                papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "%s: pdf_filter: OOM for row buffer on page %u", ctx, pagenum);
                break;
            }
            prev_w = (unsigned)w;
        }

        drv.rstartpage_cb(job, options, device, pagenum);

        for (int y = 0; y < h && page_ok && !papplJobIsCanceled(job); y++) {
            if (fread(rowbuf, 1, (size_t)w, gs) != (size_t)w) {
                papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "%s: pdf_filter: short read y=%d page %u: %s",
                    ctx, y, pagenum, strerror(errno));
                page_ok = false;
            } else {
                drv.rwriteline_cb(job, options, device, (unsigned)y, rowbuf);
            }
        }

        if (!papplJobIsCanceled(job))
            drv.rendpage_cb(job, options, device, pagenum);

        pagenum++;
        if (!page_ok || papplJobIsCanceled(job))
            break;
    }

jobs_done:
    ok = (pagenum > 0);
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
        "%s: pdf_filter: %s — %u page(s)", ctx, ok ? "ok" : "FAILED", pagenum);
    if (papplJobIsCanceled(job))
        papplLogJob(job, PAPPL_LOGLEVEL_INFO,
            "%s: pdf_filter: cancelled after %u complete page(s)", ctx, pagenum);

done:
    if (job_started)
        drv.rendjob_cb(job, options, device);

    if (gs) {
        int gs_status = pclose(gs);
        if (gs_status != 0)
            papplLogJob(job, PAPPL_LOGLEVEL_WARN,
                "%s: pdf_filter: gs exited with status %d", ctx, gs_status);
    }

    free(rowbuf);
    papplJobDeletePrintOptions(options);
    return ok;
}

/* Called from system_cb (main.c) after papplSystemCreate(). */
void register_pdf_filter(pappl_system_t *system)
{
    papplSystemAddMIMEFilter(
        system,
        "application/pdf",
        "image/pwg-raster",
        pdf_filter_cb,
        NULL);
    papplLog(system, PAPPL_LOGLEVEL_INFO,
        "pdf_filter: registered application/pdf -> image/pwg-raster");
}

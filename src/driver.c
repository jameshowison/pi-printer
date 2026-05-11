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
 * PI-SIDE BUILD NOTES (PAPPL 1.3.1)
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

/* 8×8 clustered-dot ordered dither, values 0–63.
 * Threshold = val*4+2 (range 2–254); pixel < threshold → black (bit=1). */
static const unsigned char dither8[8][8] = {
    { 24, 10, 12, 26, 35, 47, 49, 37 },
    {  8,  0,  2, 14, 45, 59, 61, 51 },
    { 22,  6,  4, 16, 43, 57, 63, 53 },
    { 30, 20, 18, 28, 33, 41, 55, 39 },
    { 34, 46, 48, 36, 25, 11, 13, 27 },
    { 44, 58, 60, 50,  9,  1,  3, 15 },
    { 42, 56, 62, 52, 23,  7,  5, 17 },
    { 32, 40, 54, 38, 31, 21, 19, 29 },
};

/* 50% threshold: 8-bit grayscale (0=black, 255=white) → 1-bit packed (1=black). */
static void threshold_row(const unsigned char *src, unsigned char *dst, unsigned w)
{
    memset(dst, 0, (w + 7u) / 8u);
    for (unsigned x = 0; x < w; x++) {
        if (src[x] < 128u)
            dst[x >> 3] |= (unsigned char)(0x80u >> (x & 7u));
    }
}

/* Ordered dither with the 8×8 clustered-dot matrix. */
static void dither_row(const unsigned char *src, unsigned char *dst,
                       unsigned w, unsigned y)
{
    unsigned row = y & 7u;
    memset(dst, 0, (w + 7u) / 8u);
    for (unsigned x = 0; x < w; x++) {
        unsigned thr = (unsigned)dither8[row][x & 7u] * 4u + 2u;
        if ((unsigned)src[x] < thr)
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
 * GS is given these dimensions via -dFIXEDMEDIA so it renders exactly the
 * printable area — no overflow at the bottom edge. */
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

    /* APT: printer-side halftoning for high print quality.
     * Forces 600 dpi so the printer can upscale from the 150 dpi TIFF input. */
    p->apt = (opts->print_quality == IPP_QUALITY_HIGH);
    if (p->apt)
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

    /* PCL raster setup:
     *   ESC E        — printer reset (clears any leftover raster state)
     *   ESC *t<N>R   — raster resolution N dpi
     *   ESC *r0F     — presentation: portrait, no rotation
     *   ESC *b2M     — compression: TIFF packbits (mode 2)
     *   ESC *r1A     — start raster at top-left of page
     * Paper/tray/duplex are already set by PJL and persist across ESC E. */
    n = snprintf(buf, sizeof(buf),
        "\033E"
        "\x1b*t%dR"
        "\x1b*r0F"
        "\x1b*b2M"
        "\x1b*r1A",
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

    /* Halftone 8-bit grayscale → 1-bit packed row in halftone_buf. */
    if (jd->resolution >= 600)
        dither_row(line, jd->halftone_buf, w, y);
    else
        threshold_row(line, jd->halftone_buf, w);

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
        "\x0c"      /* form feed — eject page */
        "\033E";    /* PCL reset */

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

static bool hl5170dn_status(pappl_printer_t *printer)
{
    /* Always set supply level first — exit criterion met even if device is busy. */
    pappl_supply_t supply;
    memset(&supply, 0, sizeof(supply));
    supply.color       = PAPPL_SUPPLY_COLOR_BLACK;
    supply.is_consumed = true;
    supply.level       = -1;   /* unknown — INFO SUPPLIES returns "?" on HL-5170DN */
    supply.type        = PAPPL_SUPPLY_TYPE_TONER_CARTRIDGE;
    strncpy(supply.description, "Black Toner Cartridge",
            sizeof(supply.description) - 1);
    papplPrinterSetSupplies(printer, 1, &supply);

    /* Open USB device for PJL back-channel query.
     * Returns NULL if device is busy (job in progress) — that's fine. */
    pappl_device_t *device = papplPrinterOpenDevice(printer);
    if (!device) {
        papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
            "status: device busy, skipping PJL poll");
        return true;
    }

    /* Send @PJL INFO STATUS query. */
    static const char query[] =
        "\033%-12345X@PJL\r\n"
        "@PJL INFO STATUS\r\n"
        "\033%-12345X";
    papplDeviceWrite(device, query, sizeof(query) - 1);
    papplDeviceFlush(device);

    /* Read FF-terminated response.  papplDeviceRead() has no timeout parameter;
     * the USB backend (libusb) handles its own read timeout (~400 ms observed
     * in Phase 0). */
    char    buf[1024];
    ssize_t bytes = papplDeviceRead(device, buf, sizeof(buf) - 1);
    papplPrinterCloseDevice(printer);

    if (bytes <= 0) {
        papplLogPrinter(printer, PAPPL_LOGLEVEL_WARN,
            "status: no response from printer (bytes=%zd)", bytes);
        return true;
    }
    buf[bytes] = '\0';

    /* Parse CODE= and ONLINE= from the response. */
    int  code   = -1;
    bool online = false;
    char *p;
    if ((p = strstr(buf, "CODE=")) != NULL)
        code = atoi(p + 5);
    if ((p = strstr(buf, "ONLINE=")) != NULL)
        online = (strncmp(p + 7, "TRUE", 4) == 0);

    papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
        "status: CODE=%d ONLINE=%s", code, online ? "TRUE" : "FALSE");

    /* Map status code range to printer reasons.
     * 40000–40999: operator-attention errors (paper empty, cover open, jam).
     * All other codes (10xxx ready, 40000=sleep per Phase 0) are non-error. */
    if (code >= 40001 && code <= 40999)
        papplPrinterSetReasons(printer, PAPPL_PREASON_OTHER, PAPPL_PREASON_NONE);
    else
        papplPrinterSetReasons(printer, PAPPL_PREASON_NONE, PAPPL_PREASON_OTHER);

    return true;
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

    return true;
}

/* ---- Phase 6A: APT photo path (Mode 1024) -------------------------------- *
 *
 * APT (Automatic Photo Tuning) lets the printer halftone 8-bit grayscale
 * data at 600 dpi equivalent from a low-resolution (≤150 dpi) input.
 * The entire page travels as one TIFF file inside a single ESC*b<N>W command.
 *
 * Manual reference: Tech_Manual_Ch2_PCL §6.3.8.
 * APT activates when: BitsPerSample=8, Compression=1 (no compression),
 * and the printer operates at 600 dpi.
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
        "-sOutputFile=- '%s' 2>/tmp/hl5170dn-gs-apt.log",
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

        /* PCL framing: reset, set 600 dpi, Mode 1024, start raster, W-command */
        char pcl[128];
        int  pcl_len = snprintf(pcl, sizeof(pcl),
            "\033E"
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
        for (int y = 0; y < h && page_ok; y++) {
            if (fread(rowbuf, 1, (size_t)w, gs) != (size_t)w) {
                papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "%s: apt_render: short read y=%d page %u: %s",
                    ctx, y, pagenum, strerror(errno));
                page_ok = false;
            } else {
                papplDeviceWrite(device, rowbuf, (size_t)w);
            }
        }

        papplDeviceWrite(device, "\x1b*rC\x0c\033E", 7);   /* end raster, eject */
        papplDeviceFlush(device);

        /* Keep PAPPL job accounting consistent */
        options->header.cupsWidth  = (unsigned)w;
        options->header.cupsHeight = (unsigned)h;
        drv->rendpage_cb(job, options, device, pagenum);

        pagenum++;
        if (!page_ok)
            break;
    }

done:
    ok = (pagenum > 0);
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
        "%s: apt_render: %s — %u page(s)", ctx, ok ? "ok" : "FAILED", pagenum);

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

        /* Phase 6A: APT path for high print quality.
         * apt_render_pdf() handles the full page loop and calls rendjob_cb
         * itself, so we skip the normal GS streaming path below. */
        if (options->print_quality == IPP_QUALITY_HIGH) {
            papplLogJob(job, PAPPL_LOGLEVEL_INFO,
                "%s: pdf_filter: using APT Mode 1024 (%d dpi input)",
                ctx, APT_INPUT_DPI);
            ok = apt_render_pdf(job, options, device, &drv, filename, ctx);
            /* rendjob_cb called inside apt_render_pdf via job_started flag */
            job_started = false;  /* prevent double-call in cleanup below */
            goto done;
        }

        int gs_w_pts, gs_h_pts;
        paper_imageable_pts(options->media.size_name, &gs_w_pts, &gs_h_pts);
        int res = jd->resolution;

        /* GS renders the PDF as 8-bit grayscale PGM to stdout.
         * Render to the imageable area dimensions (not the full page) so the
         * PCL raster fills the printable area exactly with no bottom overflow.
         * -dFIXEDMEDIA + DEVICEWIDTHPOINTS/HEIGHTPOINTS set the canvas size.
         * -dFitPage scales the PDF content to fill that canvas. */
        snprintf(gs_cmd, sizeof(gs_cmd),
            "gs -dBATCH -dNOPAUSE -dSAFE "
            "-sDEVICE=pgmraw -r%d -dFitPage "
            "-dFIXEDMEDIA -dDEVICEWIDTHPOINTS=%d -dDEVICEHEIGHTPOINTS=%d "
            "-sOutputFile=- '%s' 2>/tmp/hl5170dn-gs.log",
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

        for (int y = 0; y < h && page_ok; y++) {
            if (fread(rowbuf, 1, (size_t)w, gs) != (size_t)w) {
                papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "%s: pdf_filter: short read y=%d page %u: %s",
                    ctx, y, pagenum, strerror(errno));
                page_ok = false;
            } else {
                drv.rwriteline_cb(job, options, device, (unsigned)y, rowbuf);
            }
        }

        drv.rendpage_cb(job, options, device, pagenum);
        pagenum++;
        if (!page_ok)
            break;
    }

jobs_done:
    ok = (pagenum > 0);
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
        "%s: pdf_filter: %s — %u page(s)", ctx, ok ? "ok" : "FAILED", pagenum);

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

/* PI-SIDE BUILD NOTES (PAPPL 1.3.1)
 * If the compiler complains about field names in pappl_pr_driver_data_t:
 *   - "force_raster_type" — field may not exist; remove the assignment and
 *     rely on raster_types alone to select 1-bit black.
 *   - "identify_actions" — may be "identify_actions_supported"; check the
 *     pappl/printer.h header and rename accordingly.
 *   - source[] / type[] — if declared as "const char *" not "char[][64]",
 *     replace the strncpy calls with direct pointer assignments:
 *       data->source[0] = "tray-1";
 */

#include <pappl/pappl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>   /* unlink */
#include <limits.h>   /* INT_MAX */
#include "pjl.h"
#include "packbits.h"

/* ---- Per-job state ---------------------------------------------------- */

typedef struct {
    time_t         start_time;
    unsigned char *line_buf;       /* packbits output buffer, reused per line */
    size_t         line_buf_size;
} hl5170dn_job_t;

/* ---- Raster callbacks -------------------------------------------------- */

static bool hl5170dn_rstartjob(pappl_job_t *job, pappl_pr_options_t *options,
                                pappl_device_t *device)
{
    int resolution = (int)options->header.HWResolution[0];
    size_t bytes_per_line = (size_t)options->header.cupsBytesPerLine;

    hl5170dn_job_t *jd = calloc(1, sizeof(*jd));
    if (!jd) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "out of memory for job data");
        return false;
    }

    jd->start_time    = time(NULL);
    jd->line_buf_size = packbits_max(bytes_per_line);
    jd->line_buf      = malloc(jd->line_buf_size);
    if (!jd->line_buf) {
        free(jd);
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "out of memory for line buffer");
        return false;
    }

    papplJobSetData(job, jd);

    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
        "start job: %ddpi, %u bytes/line", resolution, (unsigned)bytes_per_line);

    /* Send PJL header.  POWERSAVE=OFF keeps the printer awake during the
     * raster render that happens before rstartpage_cb is called.
     * See plan.md §"USB keepalive", Option B. */
    pjl_write_job_header(device, resolution, /*powersave_off=*/true);

    return true;
}

static bool hl5170dn_rstartpage(pappl_job_t *job, pappl_pr_options_t *options,
                                 pappl_device_t *device, unsigned page)
{
    int resolution = (int)options->header.HWResolution[0];
    char buf[64];
    int n;

    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "start page %u at %ddpi", page, resolution);

    /* PCL raster setup (per PRD §rstartpage_cb):
     *   ESC E          — printer reset, clears any leftover raster state
     *   ESC *t<N>R     — raster resolution N dpi
     *   ESC *r0F       — presentation: portrait, no rotation
     *   ESC *b2M       — compression: TIFF packbits (mode 2)
     *   ESC *r1A       — start raster at top-left of page
     * Paper size, source, and duplex are already set by PJL; no PCL
     * paper commands here. */
    n = snprintf(buf, sizeof(buf),
        "\033E"         /* PCL reset */
        "\x1b*t%dR"     /* raster resolution */
        "\x1b*r0F"      /* presentation */
        "\x1b*b2M"      /* compression = packbits */
        "\x1b*r1A",     /* start raster */
        resolution);

    papplDeviceWrite(device, buf, (size_t)n);
    papplDeviceFlush(device);

    return true;
}

static bool hl5170dn_rwriteline(pappl_job_t *job, pappl_pr_options_t *options,
                                 pappl_device_t *device, unsigned y,
                                 const unsigned char *line)
{
    hl5170dn_job_t *jd = papplJobGetData(job);
    size_t bytes_per_line = (size_t)options->header.cupsBytesPerLine;
    char   hdr[32];
    int    hdr_len;
    size_t encoded_len;

    /* Diagnostic: log the very first line of each job so we can verify that
     * (a) rwriteline_cb is being called at all, (b) bytes_per_line is non-zero,
     * and (c) the pixel data contains non-zero bytes (actual image content).
     * Remove once printing is confirmed working. */
    if (y == 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_INFO,
            "rwriteline y=0: bytes_per_line=%zu line[0]=0x%02x line[1]=0x%02x",
            bytes_per_line, line[0], bytes_per_line > 1 ? line[1] : 0u);
    }

    encoded_len = packbits_encode(line, bytes_per_line, jd->line_buf);

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

    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "end page %u", page);
    papplDeviceWrite(device, end_page, sizeof(end_page) - 1);
    papplDeviceFlush(device);

    return true;
}

static bool hl5170dn_rendjob(pappl_job_t *job, pappl_pr_options_t *options,
                              pappl_device_t *device)
{
    hl5170dn_job_t *jd = papplJobGetData(job);

    (void)options;

    papplLogJob(job, PAPPL_LOGLEVEL_INFO, "end job (elapsed %lds)",
        (long)(time(NULL) - jd->start_time));

    /* UEL + EOJ + restore sleep mode */
    pjl_write_job_trailer(device, /*restore_powersave=*/true);

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
    (void)printer;
    /* Phase 5 will poll PJL INFO STATUS here. */
    return true;
}

/* ---- Driver registration ---------------------------------------------- */

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

    /* Do NOT memset(data, 0) here.  PAPPL calls _papplPrinterInitDriverData()
     * before invoking driver_cb, which zeroes the struct and installs proper
     * 16×16 dither matrices in data->gdither (clustered-dot) and data->pdither
     * (blue-noise).  Wiping them to zero causes all pixels with value > 0
     * (any non-black pixel) to be dropped by the dither check in
     * papplJobFilterImage, producing a blank page for anything except pure
     * black text.  We only set the fields we actually care about; PAPPL's
     * defaults for everything else are correct. */

    strncpy(data->make_and_model, "Brother HL-5170DN",
            sizeof(data->make_and_model) - 1);
    data->ppm = 21; /* HL-5170DN rated 21 ppm */

    /* Callbacks */
    data->identify_cb   = hl5170dn_identify;
    data->status_cb     = hl5170dn_status;
    data->rstartjob_cb  = hl5170dn_rstartjob;
    data->rstartpage_cb = hl5170dn_rstartpage;
    data->rwriteline_cb = hl5170dn_rwriteline;
    data->rendpage_cb   = hl5170dn_rendpage;
    data->rendjob_cb    = hl5170dn_rendjob;

    /* Phase 1: 300 dpi only (Investigation 2: 600 dpi takes 45s for photos,
     * which exceeds the safe USB-keepalive window).  Phase 2 adds 600. */
    data->num_resolution   = 1;
    data->x_resolution[0]  = 300;
    data->y_resolution[0]  = 300;
    data->x_default        = 300;
    data->y_default        = 300;

    /* Force 1-bit black raster; PAPPL halftones for us. */
    data->raster_types      = PAPPL_PWG_RASTER_TYPE_BLACK_1;
    data->force_raster_type = PAPPL_PWG_RASTER_TYPE_BLACK_1;

    /* Monochrome only. */
    data->color_supported = PAPPL_COLOR_MODE_MONOCHROME;
    data->color_default   = PAPPL_COLOR_MODE_MONOCHROME;

    /* PAPPL 1.3.1 calls _papplContentString / _papplScalingString with these
     * values and passes the result directly to strlcpy — a NULL return (from
     * value 0) causes a SIGSEGV.  Set valid defaults for all three. */
    data->content_default = PAPPL_CONTENT_AUTO;
    data->scaling_default = PAPPL_SCALING_AUTO;
    data->quality_default = IPP_QUALITY_NORMAL;

    /* No duplex in Phase 1. */
    data->duplex        = PAPPL_DUPLEX_NONE;
    data->input_face_up = false;

    /* Identify: declare SOUND but the callback is a no-op. */
    data->identify_default   = PAPPL_IDENTIFY_ACTIONS_SOUND;
    data->identify_supported = PAPPL_IDENTIFY_ACTIONS_SOUND;

    /* Media: Letter only, from tray-1 and by-pass-tray. */
    data->num_media = 1;
    data->media[0]  = "na_letter_8.5x11in";

    memset(&data->media_default, 0, sizeof(data->media_default));
    strncpy(data->media_default.size_name, "na_letter_8.5x11in",
            sizeof(data->media_default.size_name) - 1);
    data->media_default.size_width    = 21590; /* 8.5" in hundredths of mm */
    data->media_default.size_length   = 27940; /* 11" in hundredths of mm */
    data->media_default.left_margin   = 500;
    data->media_default.right_margin  = 500;
    data->media_default.top_margin    = 500;
    data->media_default.bottom_margin = 500;
    strncpy(data->media_default.source, "tray-1",
            sizeof(data->media_default.source) - 1);
    strncpy(data->media_default.type, "stationery",
            sizeof(data->media_default.type) - 1);

    /* Sources */
    data->num_source = 2;
    data->source[0]  = "tray-1";
    data->source[1]  = "by-pass-tray";

    /* media_ready: tray-1 has Letter loaded; by-pass-tray inherits same */
    data->media_ready[0] = data->media_default;
    data->media_ready[1] = data->media_default;
    strncpy(data->media_ready[1].source, "by-pass-tray",
            sizeof(data->media_ready[1].source) - 1);

    /* Media types */
    data->num_type = 1;
    data->type[0]  = "stationery";

    /* Output bin — PAPPL 1.3.1 calls strlcpy(options->output_bin,
     * data->bin[data->bin_default]) without a num_bin > 0 guard. */
    data->num_bin    = 1;
    data->bin[0]     = "face-down";
    data->bin_default = 0;

    return true;
}

/* ---- PDF → PCL filter (via Ghostscript) --------------------------------- *
 *
 * PAPPL 1.3.1 has no papplSystemAddMIMEFilter(); this function exists only
 * when built against PAPPL 1.4+.  The Makefile currently links whatever
 * version pkg-config finds; see bring-up-notes.md §1 for the source-build
 * steps that install 1.4.x to /usr/local.
 *
 * Design: iPhone AirPrint sends application/pdf.  PAPPL's built-in raster
 * filters handle PWG/Apple raster and JPEG/PNG natively.  For PDF we
 * register a MIME filter here that:
 *   1. Shells out to ghostscript to render the PDF as 1-bit PBM ("pbmraw")
 *      — one P4 block per page.
 *   2. Calls our own raster callbacks directly, page by page, to emit the
 *      packbits-encoded PCL wrapped in PJL.
 *
 * The PAPPL docs (§ "Processing Jobs") say: "A raster filter that needs to
 * print more than one image must use the raster callback functions in the
 * pappl_pr_driver_data_t structure directly."  We do exactly that.
 *
 * Why PBM (pbmraw) and not pwgraster?
 *   - PBM is trivial to parse (P4 magic + "w h" header + raw bits, MSB=left).
 *   - Our rwriteline_cb already expects 1-bit, MSB-first packed bytes —
 *     exactly what gs pbmraw produces.  No intermediate colour conversion
 *     or raster library needed.
 *
 * Build note: papplJobCreatePrintOptions is confirmed to take (job, num_pages)
 * in PAPPL ≤1.4.  The third arg (false) is speculative — remove it if the
 * compiler complains "too many arguments".  If you get "too few", check the
 * local pappl/job.h for the actual third parameter type.
 */

static bool pdf_filter_cb(pappl_job_t *job, pappl_device_t *device, void *cbdata)
{
    const char          *filename = papplJobGetFilename(job);
    pappl_printer_t     *printer  = papplJobGetPrinter(job);
    pappl_pr_driver_data_t drv;
    pappl_pr_options_t  *options;
    char                 gs_cmd[4096];
    char                 pbm_path[256];
    char                 line[256];
    FILE                *pbm = NULL;
    unsigned             pagenum = 0;
    bool                 ok = false;

    (void)cbdata;

    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
        "pdf_filter: converting '%s' via gs pbmraw", filename);

    /* Build print options from the job's IPP attributes.
     * INT_MAX for num_pages = "unknown", which preserves the driver's duplex
     * default rather than forcing single-sided (see PAPPL issue #60). */
    options = papplJobCreatePrintOptions(job, (unsigned)INT_MAX, false);
    if (!options) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
            "pdf_filter: papplJobCreatePrintOptions failed");
        return false;
    }

    /* Force 300 dpi.  Phase 0 investigation showed 600 dpi photo render
     * takes ~45s on Pi 3B+ — beyond the USB keepalive window.  300 dpi is
     * safe for both text and photos. */
    options->header.HWResolution[0] = 300;
    options->header.HWResolution[1] = 300;

    /* Get driver callbacks so we can drive the raster pipeline ourselves. */
    papplPrinterGetDriverData(printer, &drv);

    /* Temp output file — named by job ID to avoid collisions if two jobs
     * run concurrently (unlikely on a single-USB printer, but be safe). */
    snprintf(pbm_path, sizeof(pbm_path),
             "/tmp/hl5170dn-pdf-%d.pbm", (int)papplJobGetID(job));

    /* gs renders the PDF as 1-bit monochrome PBM.
     *   -dSAFE   : prevents PostScript programs in the PDF from calling
     *              system(), accessing the network, or writing arbitrary files.
     *   -dFitPage: scales each page to fill the paper without clipping.
     *   -r300    : 300 dpi output — matches the driver's Phase 1 resolution.
     * Stderr is redirected so gs diagnostic output doesn't corrupt the PBM
     * stream; inspect /tmp/hl5170dn-gs.log when debugging. */
    snprintf(gs_cmd, sizeof(gs_cmd),
        "gs -dBATCH -dNOPAUSE -dSAFE "
        "-sDEVICE=pbmraw -r300 -dFitPage -sPAPERSIZE=letter "
        "-sOutputFile='%s' '%s' 2>/tmp/hl5170dn-gs.log",
        pbm_path, filename);

    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "pdf_filter: gs cmd: %s", gs_cmd);

    if (system(gs_cmd) != 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
            "pdf_filter: gs failed (exit non-zero) — "
            "inspect /tmp/hl5170dn-gs.log");
        goto done;
    }

    pbm = fopen(pbm_path, "rb");
    if (!pbm) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
            "pdf_filter: cannot open gs output '%s': %s",
            pbm_path, strerror(errno));
        goto done;
    }

    /* Send the PJL job header once, before any page data. */
    if (!drv.rstartjob_cb(job, options, device)) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
            "pdf_filter: rstartjob_cb failed");
        goto done;
    }

    /* Each P4 block in the PBM file is one page of the PDF.
     * fgets reads lines; the PBM binary pixel data follows immediately
     * after the header, so we must switch to fread for pixel rows. */
    while (fgets(line, sizeof(line), pbm)) {
        int           w, h;
        size_t        rowbytes;
        unsigned char *rowbuf;
        bool          page_ok = true;

        /* Skip until we see the P4 magic (raw PBM). */
        if (strncmp(line, "P4", 2) != 0)
            continue;

        /* Skip optional comment lines after the magic. */
        do {
            if (!fgets(line, sizeof(line), pbm))
                goto jobs_done;   /* EOF between magic and dimensions */
        } while (line[0] == '#');

        /* "line" now holds "<width> <height>\n". */
        if (sscanf(line, "%d %d", &w, &h) != 2 || w <= 0 || h <= 0) {
            papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                "pdf_filter: bad PBM dimensions on page %u", pagenum);
            break;
        }

        rowbytes = (size_t)((w + 7) / 8);   /* bits packed, MSB first */

        /* Patch options so the raster callbacks know the page geometry. */
        options->header.cupsWidth        = (unsigned)w;
        options->header.cupsHeight       = (unsigned)h;
        options->header.cupsBytesPerLine = (unsigned)rowbytes;

        papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
            "pdf_filter: page %u: %dx%d px, %zu bytes/line",
            pagenum, w, h, rowbytes);

        rowbuf = malloc(rowbytes);
        if (!rowbuf) {
            papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                "pdf_filter: out of memory for row buffer (page %u)", pagenum);
            page_ok = false;
        } else {
            drv.rstartpage_cb(job, options, device, pagenum);

            for (int y = 0; y < h && page_ok; y++) {
                if (fread(rowbuf, 1, rowbytes, pbm) != rowbytes) {
                    papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                        "pdf_filter: short read at y=%d page %u: %s",
                        y, pagenum, strerror(errno));
                    page_ok = false;
                } else {
                    drv.rwriteline_cb(job, options, device, (unsigned)y, rowbuf);
                }
            }

            drv.rendpage_cb(job, options, device, pagenum);
            free(rowbuf);
        }

        pagenum++;
        if (!page_ok)
            break;
    }

jobs_done:
    drv.rendjob_cb(job, options, device);
    ok = (pagenum > 0);

    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
        "pdf_filter: %s — %u page(s)", ok ? "ok" : "FAILED", pagenum);

done:
    if (pbm) { fclose(pbm); pbm = NULL; }
    unlink(pbm_path);   /* harmless if gs failed and file was never created */
    papplJobDeletePrintOptions(options);
    return ok;
}

/* Called from system_cb (main.c) after papplSystemCreate().
 * Must be called before papplSystemRun() / papplMainloop(). */
void register_pdf_filter(pappl_system_t *system)
{
    papplSystemAddMIMEFilter(
        system,
        "application/pdf",   /* srctype: iPhone AirPrint sends this */
        "image/pwg-raster",  /* dsttype: signals raster-capable filter;
                              * our filter drives the raster callbacks
                              * directly rather than emitting PWG bytes */
        pdf_filter_cb,
        NULL);
    papplLog(system, PAPPL_LOGLEVEL_INFO,
        "pdf_filter: registered application/pdf -> image/pwg-raster");
}

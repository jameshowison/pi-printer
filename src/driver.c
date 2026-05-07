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
        "\x1bE"         /* PCL reset */
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

    (void)y; /* used only for tracing; omit for Phase 1 */

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
        "\x1bE";    /* PCL reset */

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

    memset(data, 0, sizeof(*data));

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

    /* No duplex, no copies in Phase 1. */
    data->duplex     = PAPPL_DUPLEX_NONE;
    data->has_copies = false;
    data->input_face_up = false;

    /* Identify: declare SOUND but the callback is a no-op. */
    data->identify_actions = PAPPL_IDENTIFY_ACTIONS_SOUND;

    /* Media: Letter only, from tray-1 and by-pass-tray. */
    data->num_media = 1;
    strncpy(data->media[0].size_name, "na_letter_8.5x11in",
            sizeof(data->media[0].size_name) - 1);
    data->media[0].size_width   = 21590; /* 8.5" in hundredths of mm */
    data->media[0].size_length  = 27940; /* 11" in hundredths of mm */
    data->media[0].left_margin  = 500;
    data->media[0].right_margin = 500;
    data->media[0].top_margin   = 500;
    data->media[0].bottom_margin = 500;
    strncpy(data->media[0].source, "tray-1",
            sizeof(data->media[0].source) - 1);
    strncpy(data->media[0].type, "stationery",
            sizeof(data->media[0].type) - 1);

    data->media_default = data->media[0];

    /* Sources */
    data->num_source = 2;
    strncpy(data->source[0], "tray-1",      sizeof(data->source[0]) - 1);
    strncpy(data->source[1], "by-pass-tray", sizeof(data->source[1]) - 1);

    /* media_ready: tray-1 has Letter loaded; by-pass-tray inherits same */
    data->media_ready[0] = data->media_default;
    data->media_ready[1] = data->media_default;
    strncpy(data->media_ready[1].source, "by-pass-tray",
            sizeof(data->media_ready[1].source) - 1);

    /* Media types */
    data->num_type = 1;
    strncpy(data->type[0], "stationery", sizeof(data->type[0]) - 1);

    return true;
}

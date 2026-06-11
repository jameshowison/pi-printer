#include "pjl.h"
#include <stdio.h>
#include <string.h>

/* Universal Exit Language sequence — transitions printer to PJL mode.
 * The literal '%' must NOT pass through snprintf; write it directly. */
static const char UEL[] = "\x1b%-12345X";
#define UEL_LEN (sizeof(UEL) - 1)

void pjl_write_job_header(pappl_device_t *dev, const pjl_job_params_t *p)
{
    char buf[512];
    int  n;

    papplDeviceWrite(dev, UEL, UEL_LEN);

    /* Block 1: @PJL preamble, POWERSAVE, RESOLUTION, ECONOMODE, DUPLEX */
    n = snprintf(buf, sizeof(buf),
        "@PJL\r\n"
        "%s"
        "@PJL SET RESOLUTION=%d\r\n"
        "@PJL SET ECONOMODE=%s\r\n"
        "@PJL SET DUPLEX=%s\r\n",
        p->powersave_off ? "@PJL SET POWERSAVE=OFF\r\n" : "",
        p->resolution,
        p->economode ? "ON" : "OFF",
        p->duplex    ? "ON" : "OFF");
    papplDeviceWrite(dev, buf, (size_t)n);

    /* BINDING only when DUPLEX=ON, and must follow DUPLEX */
    if (p->duplex && p->binding) {
        n = snprintf(buf, sizeof(buf), "@PJL SET BINDING=%s\r\n", p->binding);
        papplDeviceWrite(dev, buf, (size_t)n);
    }

    /* Block 2: remaining settings */
    n = snprintf(buf, sizeof(buf),
        "@PJL SET SOURCETRAY=%s\r\n"
        "@PJL SET MEDIATYPE=%s\r\n"
        "@PJL SET COPIES=%d\r\n"
        "@PJL SET LPARM : PCL PAPER=%s\r\n",
        p->source    ? p->source    : "AUTO",
        p->mediatype ? p->mediatype : "REGULAR",
        p->copies > 0 ? p->copies : 1,
        p->paper     ? p->paper     : "LETTER");
    papplDeviceWrite(dev, buf, (size_t)n);

    /* APT: enable printer-side halftoning for 8-bit grayscale TIFF input. */
    if (p->apt) {
        n = snprintf(buf, sizeof(buf),
            "@PJL SET APT=ON\r\n"
            "@PJL SET IMAGEADAPT=ON\r\n");
        papplDeviceWrite(dev, buf, (size_t)n);
    }

    /* Enable unsolicited status so the printer pushes back-channel updates during the job.
     * DEVICE=VERBOSE fires on any status change; TIMED=5 fires every 5 seconds;
     * PAGE=ON fires on each physical page completion (manual §7.6.3 says EOJ
     * resets the counter; empirical behaviour is still to be confirmed by the
     * raw-logging reader thread).  Drained continuously by a dedicated reader
     * thread in pdf_filter_cb so events are captured in real time, not buffered
     * up and drained in bursts. */
    n = snprintf(buf, sizeof(buf),
        "@PJL USTATUS DEVICE = VERBOSE\r\n"
        "@PJL USTATUS TIMED = 5\r\n"
        "@PJL USTATUS PAGE = ON\r\n");
    papplDeviceWrite(dev, buf, (size_t)n);

    n = snprintf(buf, sizeof(buf), "@PJL ENTER LANGUAGE=PCL\r\n");
    papplDeviceWrite(dev, buf, (size_t)n);

    papplDeviceFlush(dev);
}

void pjl_write_job_trailer(pappl_device_t *dev, bool restore_powersave)
{
    char buf[128];
    int  n;

    /* UEL exits PCL back to PJL. */
    papplDeviceWrite(dev, UEL, UEL_LEN);

    n = snprintf(buf, sizeof(buf),
        "@PJL USTATUSOFF\r\n"
        "@PJL EOJ\r\n"
        "%s",
        restore_powersave ? "@PJL SET POWERSAVE=ON\r\n" : "");
    papplDeviceWrite(dev, buf, (size_t)n);

    /* Final UEL closes the PJL session. */
    papplDeviceWrite(dev, UEL, UEL_LEN);
    papplDeviceFlush(dev);
}

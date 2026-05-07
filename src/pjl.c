#include "pjl.h"
#include <stdio.h>
#include <string.h>

/* Universal Exit Language sequence — transitions printer to PJL mode. */
#define UEL "\x1b%-12345X"

void pjl_write_job_header(pappl_device_t *dev, int resolution,
                          bool powersave_off)
{
    char buf[512];
    int  n;

    /* All PJL SETs in a single UEL block.  POWERSAVE=OFF comes first so
     * the printer stays awake during the raster render that follows.
     * Field order matches Investigation 1 capture for easy diff. */
    n = snprintf(buf, sizeof(buf),
        UEL
        "@PJL\r\n"
        "%s"   /* POWERSAVE=OFF if requested */
        "@PJL SET RESOLUTION=%d\r\n"
        "@PJL SET ECONOMODE=OFF\r\n"
        "@PJL SET DUPLEX=OFF\r\n"
        "@PJL SET SOURCETRAY=AUTO\r\n"
        "@PJL SET MEDIATYPE=REGULAR\r\n"
        "@PJL SET COPIES=1\r\n"
        "@PJL SET LPARM : PCL PAPER=LETTER\r\n"
        "@PJL ENTER LANGUAGE=PCL\r\n",
        powersave_off ? "@PJL SET POWERSAVE=OFF\r\n" : "",
        resolution);

    papplDeviceWrite(dev, buf, (size_t)n);
    papplDeviceFlush(dev);
}

void pjl_write_job_trailer(pappl_device_t *dev, bool restore_powersave)
{
    char buf[128];
    int  n;

    /* UEL exits PCL, EOJ closes the job, optional POWERSAVE=ON restores
     * sleep mode, final UEL closes the PJL session. */
    n = snprintf(buf, sizeof(buf),
        UEL
        "@PJL EOJ\r\n"
        "%s"    /* POWERSAVE=ON if restoring */
        UEL,
        restore_powersave ? "@PJL SET POWERSAVE=ON\r\n" : "");

    papplDeviceWrite(dev, buf, (size_t)n);
    papplDeviceFlush(dev);
}

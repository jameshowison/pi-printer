#include "pjl.h"
#include <stdio.h>
#include <string.h>

/* Universal Exit Language sequence — transitions printer to PJL mode.
 * The literal '%' must NOT pass through snprintf; write it directly. */
static const char UEL[] = "\x1b%-12345X";
#define UEL_LEN (sizeof(UEL) - 1)

void pjl_write_job_header(pappl_device_t *dev, int resolution,
                          bool powersave_off)
{
    char buf[512];
    int  n;

    /* UEL written directly — snprintf would misinterpret the '%' in %-12345X. */
    papplDeviceWrite(dev, UEL, UEL_LEN);

    n = snprintf(buf, sizeof(buf),
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

    /* UEL exits PCL back to PJL. */
    papplDeviceWrite(dev, UEL, UEL_LEN);

    n = snprintf(buf, sizeof(buf),
        "@PJL EOJ\r\n"
        "%s",    /* POWERSAVE=ON if restoring */
        restore_powersave ? "@PJL SET POWERSAVE=ON\r\n" : "");

    papplDeviceWrite(dev, buf, (size_t)n);

    /* Final UEL closes the PJL session. */
    papplDeviceWrite(dev, UEL, UEL_LEN);
    papplDeviceFlush(dev);
}

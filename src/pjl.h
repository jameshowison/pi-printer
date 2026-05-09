#pragma once
#include <pappl/pappl.h>
#include <stdbool.h>

/* All per-job PJL settings derived from IPP attributes. */
typedef struct {
    int          resolution;     /* 300, 600, or 1200 */
    bool         powersave_off;  /* true → @PJL SET POWERSAVE=OFF */
    bool         duplex;         /* true → DUPLEX=ON */
    const char  *binding;        /* "LONGEDGE" or "SHORTEDGE" (duplex only) */
    const char  *paper;          /* PJL LPARM:PCL PAPER: "LETTER", "A4", … */
    const char  *source;         /* PJL SOURCETRAY: "AUTO", "TRAY1", "MP" */
    const char  *mediatype;      /* PJL MEDIATYPE: "REGULAR", "THICK", … */
    bool         economode;      /* true → ECONOMODE=ON */
    int          copies;
    bool         apt;            /* true → APT=ON + IMAGEADAPT=ON (Mode 1024) */
} pjl_job_params_t;

/* Emit UEL + all PJL SET commands + ENTER LANGUAGE=PCL.  Flushes. */
void pjl_write_job_header(pappl_device_t *dev, const pjl_job_params_t *p);

/* Emit UEL + EOJ + optional POWERSAVE=ON restore.  Flushes. */
void pjl_write_job_trailer(pappl_device_t *dev, bool restore_powersave);

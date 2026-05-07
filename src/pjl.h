#pragma once
#include <pappl/pappl.h>
#include <stdbool.h>

/* Emit PJL preamble: POWERSAVE=OFF (if requested), RESOLUTION, all per-job
 * PJL SETs, then ENTER LANGUAGE=PCL.  Flushes before returning. */
void pjl_write_job_header(pappl_device_t *dev, int resolution,
                          bool powersave_off);

/* Emit UEL + EOJ.  If restore_powersave, also re-enables sleep mode. */
void pjl_write_job_trailer(pappl_device_t *dev, bool restore_powersave);

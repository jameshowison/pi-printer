#include <pappl/pappl.h>

#define APP_VERSION  "0.1.0"
#define DRIVER_NAME  "hl5170dn"
#define PRINTER_NAME "hl5170dn"

/* USB URI confirmed by Investigation 3 (papplDeviceRead probe). */
#define DEVICE_URI   "usb://Brother/HL-5170DN%20series?serial=L4J624176"

/* Defined in driver.c */
extern bool driver_cb(pappl_system_t *system, const char *driver_name,
    const char *device_uri, const char *device_id,
    pappl_pr_driver_data_t *data, ipp_t **attrs, void *cbdata);

static pappl_system_t *system_cb(int num_options, cups_option_t *options,
                                  void *data)
{
    pappl_system_t  *system;
    pappl_printer_t *printer;

    (void)num_options;
    (void)options;
    (void)data;

    system = papplSystemCreate(
        PAPPL_SOPTIONS_NONE,
        "hl5170dn-printer-app",
        8000,
        "_print,_universal",
        NULL,                    /* spooldir: use PAPPL default */
        "-",                     /* logfile: stderr (captured by journald) */
        PAPPL_LOGLEVEL_DEBUG,
        NULL);                   /* no authentication */

    if (!system)
        return NULL;

    papplSystemAddListeners(system, NULL);

    printer = papplPrinterCreate(system, 0, PRINTER_NAME, DRIVER_NAME,
                                 NULL, DEVICE_URI);
    if (!printer)
        papplLog(system, PAPPL_LOGLEVEL_WARN,
            "papplPrinterCreate failed — printer may already exist");

    return system;
}

int main(int argc, char *argv[])
{
    static pappl_pr_driver_t drivers[] = {
        { DRIVER_NAME, "Brother HL-5170DN", NULL, NULL }
    };

    return papplMainloop(argc, argv,
        APP_VERSION,
        NULL,           /* footer_html */
        1, drivers,     /* num_drivers, drivers[] */
        NULL,           /* autoadd_cb */
        driver_cb,
        NULL,           /* subcmd_name */
        NULL,           /* subcmd_cb */
        system_cb,
        NULL);          /* cbdata */
}

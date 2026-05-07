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

/* File-scope so system_cb can reference it when registering drivers. */
static pappl_pr_driver_t drivers[] = {
    { DRIVER_NAME, "Brother HL-5170DN", NULL, NULL }
};

static pappl_system_t *system_cb(int num_options, cups_option_t *options,
                                  void *data)
{
    pappl_system_t  *system;
    pappl_printer_t *printer;

    (void)num_options;
    (void)options;
    (void)data;

    /* PAPPL_SOPTIONS_WEB_INTERFACE  — admin UI at /
     * PAPPL_SOPTIONS_WEB_LOG        — live log viewer at /logs
     * PAPPL_SOPTIONS_WEB_REMOTE     — accept admin requests from non-localhost
     *                                 (otherwise the Mac can read pages but
     *                                 not change settings).
     * Without these, papplSystemCreate registers no HTTP routes and `/` 404s. */
    system = papplSystemCreate(
        PAPPL_SOPTIONS_WEB_INTERFACE | PAPPL_SOPTIONS_WEB_LOG | PAPPL_SOPTIONS_WEB_REMOTE,
        "hl5170dn-printer-app",
        8000,
        "_print,_universal",
        NULL,                    /* spooldir: use PAPPL default */
        "-",                     /* logfile: stderr (captured by journald) */
        PAPPL_LOGLEVEL_DEBUG,
        NULL,                    /* no authentication */
        false);                  /* tls_only */

    if (!system)
        return NULL;

    papplSystemAddListeners(system, NULL);

    /* papplMainloop registers driver_cb AFTER system_cb returns, so
     * papplPrinterCreate would fail with "no driver callback set".
     * Register it here first; papplMainloop's redundant registration is harmless. */
    papplSystemSetPrinterDrivers(system, 1, drivers, NULL, NULL, driver_cb, NULL);

    printer = papplPrinterCreate(system, 0, PRINTER_NAME, DRIVER_NAME,
                                 NULL, DEVICE_URI);
    if (!printer)
        papplLog(system, PAPPL_LOGLEVEL_WARN,
            "papplPrinterCreate failed — printer may already exist");

    return system;
}

/* PAPPL 1.4 calls papplLocGetString from its default HTML footer; without
 * registered localisation data that NULL-derefs in cupsArrayFind. Bypass
 * the localised path by supplying our own footer HTML literal. */
#define APP_FOOTER_HTML \
    "<a class=\"btn\" href=\"https://github.com/jameshowison/pi-printer\">" \
    "hl5170dn-printer-app v" APP_VERSION "</a>"

int main(int argc, char *argv[])
{
    return papplMainloop(argc, argv,
        APP_VERSION,
        APP_FOOTER_HTML,/* footer_html — non-NULL to bypass PAPPL's
                           localised default footer (crashes in 1.4 when
                           no localisation strings are registered). */
        1, drivers,     /* num_drivers, drivers[] */
        NULL,           /* autoadd_cb */
        driver_cb,
        NULL,           /* subcmd_name */
        NULL,           /* subcmd_cb */
        system_cb,
        NULL,           /* usage_cb */
        NULL);          /* cbdata */
}

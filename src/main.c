#include <pappl/pappl.h>
#include <unistd.h>

#define APP_VERSION  "0.5.0"
#ifdef GIT_HASH
#  define DRIVER_VERSION  APP_VERSION "-" GIT_HASH
#else
#  define DRIVER_VERSION  APP_VERSION
#endif
/* IPP_FIRMWARE_VERSION is what we publish to papplMainloop and therefore to the IPP
 * attribute printer-firmware-string-version. It MUST stay stable across
 * rebuilds — macOS treats a changed firmware-version as a printer update and
 * prompts for admin auth to refresh the driver. Bump APP_VERSION only on a
 * deliberate release. The git hash still appears in the footer (visible on
 * the supplies page) via DRIVER_VERSION below. */
#define IPP_FIRMWARE_VERSION  APP_VERSION
#define DRIVER_NAME  "hl5170dn"
#define PRINTER_NAME "hl5170dn"

/* USB URI confirmed by Investigation 3 (papplDeviceRead probe). */
#define DEVICE_URI   "usb://Brother/HL-5170DN%20series?serial=L4J624176"

/* Defined in driver.c */
extern bool driver_cb(pappl_system_t *system, const char *driver_name,
    const char *device_uri, const char *device_id,
    pappl_pr_driver_data_t *data, ipp_t **attrs, void *cbdata);
extern void register_pdf_filter(pappl_system_t *system);
extern void register_supply_reset_route(pappl_system_t *system,
    pappl_printer_t *printer);

/* File-scope so system_cb can reference it when registering drivers. */
static pappl_pr_driver_t drivers[] = {
    { DRIVER_NAME, "Brother HL-5170DN", NULL, NULL }
};

/* Called by PAPPL inside papplPrinterCreate — for both first-boot creation
 * and state-file restore.  Registers the supplies web route and overrides
 * the PAPPL-default navicon with our custom 48×48 printer image. */
static void printer_create_cb(pappl_printer_t *printer, void *data)
{
    pappl_system_t *system = papplPrinterGetSystem(printer);

    (void)data;

    register_supply_reset_route(system, printer);

    papplSystemAddResourceFile(system, "/navicon.png", "image/png",
                               "/usr/local/share/hl5170dn/icon-48.png");
}

static pappl_system_t *system_cb(int num_options, cups_option_t *options,
                                  void *data)
{
    pappl_system_t  *system;
    const char      *xdg;
    char             statepath[512];
    bool             has_state;

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
        PAPPL_SOPTIONS_WEB_INTERFACE | PAPPL_SOPTIONS_WEB_LOG | PAPPL_SOPTIONS_WEB_REMOTE | PAPPL_SOPTIONS_NO_TLS,
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

    papplLog(system, PAPPL_LOGLEVEL_INFO, "hl5170dn-printer-app %s starting", DRIVER_VERSION);

    papplSystemAddListeners(system, NULL);

    /* papplMainloop registers driver_cb AFTER system_cb returns, so
     * papplPrinterCreate would fail with "no driver callback set".
     * Register it here first; papplMainloop's redundant registration is harmless.
     * printer_create_cb is wired in so PAPPL calls it for every papplPrinterCreate,
     * including the one inside _papplSystemLoadState when restoring from the
     * state file. */
    papplSystemSetPrinterDrivers(system, 1, drivers, NULL, printer_create_cb,
                                 driver_cb, NULL);

    /* Phase 2: wire PDF -> PWG raster via Ghostscript.  Must be called
     * BEFORE papplPrinterCreate so that document-format-supported is built
     * with application/pdf in system->filters (printer-driver.c walks the
     * filter list at creation time; adding filters afterwards has no effect
     * on the already-frozen attribute).  Requires PAPPL 1.4+. */
    register_pdf_filter(system);

    /* Only create the printer on first-boot (no persistent state yet).
     * When state exists, PAPPL's mainloop restores the printer from the state
     * file — applying saved Location, DNSSDName, media defaults, etc. — by
     * calling papplPrinterCreate inside _papplSystemLoadState, which triggers
     * printer_create_cb above.  Creating the printer here when state already
     * exists causes a duplicate-printer error and silently discards all saved
     * settings. */
    xdg       = getenv("XDG_CONFIG_HOME");
    has_state = false;
    if (xdg) {
        snprintf(statepath, sizeof(statepath), "%s/hl5170dn-printer-app.state", xdg);
        has_state = (access(statepath, F_OK) == 0);
    }

    if (!has_state) {
        pappl_printer_t *printer = papplPrinterCreate(system, 0, PRINTER_NAME,
                                       DRIVER_NAME, NULL, DEVICE_URI);
        if (!printer)
            papplLog(system, PAPPL_LOGLEVEL_WARN,
                "papplPrinterCreate failed — printer may already exist");
    }

    return system;
}

/* PAPPL 1.4 calls papplLocGetString from its default HTML footer; without
 * registered localisation data that NULL-derefs in cupsArrayFind. Bypass
 * the localised path by supplying our own footer HTML literal. */
#define APP_FOOTER_HTML \
    "<a class=\"btn\" href=\"https://github.com/jameshowison/pi-printer\">" \
    "hl5170dn-printer-app v" DRIVER_VERSION "</a>"

int main(int argc, char *argv[])
{
    return papplMainloop(argc, argv,
        IPP_FIRMWARE_VERSION,
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

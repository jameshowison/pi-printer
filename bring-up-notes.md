# PAPPL bring-up notes

Things we learned during Phase 1 bring-up that aren't in the PRD, the
PAPPL docs, or the Brother manual. Each one cost real debugging time
on first contact and is cheap to surface here for the next person (or
agent) hitting the same wall.

These are also the entries the agent-coding journal will draw from
when we ship.

## 1. PAPPL 1.3.1 (apt) is not enough — source-build 1.4.x

Phase 0 Investigation 4 originally concluded "apt 1.3.1 is sufficient
for Phases 1–5." That held until the iPhone-blank-page debug surfaced
the real blocker: **PAPPL 1.3.1 has no clean way to register PDF as a
supported input format**. iPhone AirPrint sends `application/pdf`, the
driver's bring-up log says `JPEG is supported, PDF is not supported`,
and PAPPL silently produces an empty raster pipeline that ejects a
blank page.

PAPPL 1.4 added `papplSystemAddMIMEFilter()`, which is the documented
mechanism for wiring PDF→PWG conversion (Ghostscript) into a driver's
pipeline. The Ghostscript Printer Application
(`OpenPrinting/ghostscript-printer-app`) is the reference
implementation worth reading before writing the call.

Source build is straightforward on Pi 3B+:

```bash
cd ~ && git clone https://github.com/michaelrsweet/pappl.git
cd pappl && git checkout v1.4.10        # or whatever's latest 1.4.x
./configure --prefix=/usr/local
make -j4 && sudo make install && sudo ldconfig
```

Then point the Makefile at the new headers/libs:

```make
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$$PKG_CONFIG_PATH
```

(Or set in shell before running `make`.) Verify with
`pkg-config --modversion pappl` (expect 1.4.10) and `ldd ./binary | grep
pappl` (expect `/usr/local/lib/libpappl.so.1`, not
`/usr/lib/.../libpappl1t64.so`).

Recommend leaving the apt `libpappl1t64` runtime installed — other
things on the box may depend on it — and just removing `libpappl-dev`
to avoid header-path ambiguity. The runtime linker prefers
`/usr/local/lib` over `/usr/lib` if `LD_LIBRARY_PATH` includes it
(or if you set `RPATH` in the binary, or if `/etc/ld.so.conf.d/`
includes `/usr/local/lib`).

## 2. The web UI is off by default

`papplSystemCreate(PAPPL_SOPTIONS_NONE, ...)` registers no HTTP
routes. The listener accepts the connection and returns 404 for `/`.
This is silent — there's no warning that the web UI is disabled.

The right flags for a typical "I want the admin page on :8000" setup
are `PAPPL_SOPTIONS_WEB_INTERFACE | PAPPL_SOPTIONS_WEB_LOG | PAPPL_SOPTIONS_WEB_REMOTE`.

Note **not** `PAPPL_SOPTIONS_REMOTE_ADMIN` — that name doesn't exist
in either 1.3.1 or 1.4.10. Verify against the local header:

```bash
grep PAPPL_SOPTIONS /usr/local/include/pappl/system.h
```

There's also a `PAPPL_SOPTIONS_STANDARD` preset that ORs the common
admin flags together. Worth knowing if you want the full
admin/network/security/TLS bundle rather than picking individual flags.

## 3. `papplMainloop`'s `footer_html=NULL` crashes the web UI in 1.4

When PAPPL renders the standard HTML footer (called from
`_papplPrinterWebHome` and likely other handlers), it calls
`papplClientHTMLFooter` → `papplLocGetString` → libcups
`cupsArrayFind` → `strcmp(NULL, ...)`. Segfault.

This happens because PAPPL 1.4's localisation array isn't populated by
`papplSystemCreate` automatically — there's a separate call needed
(`papplSystemAddStrings()` or similar — verify against the local
header). The default-footer code path doesn't guard against an empty
localisation table.

Two fixes:

1. **Workaround**: pass a non-NULL string for `papplMainloop`'s
   `footer_html` argument. PAPPL emits that literal instead of taking
   the localised path. One-line fix.
2. **Proper fix**: register localisation data in `system_cb` after
   `papplSystemCreate`. We're using the workaround until something
   else forces us to look at localisation.

Backtrace for the journal:

```
#0  strcmp(NULL, ...)
#1  libcups.so internal compare callback
#2  cupsArrayFind
#3  papplLocGetString
#4  papplClientHTMLFooter
#5  _papplPrinterWebHome
#6  _papplClientProcessHTTP
#7  _papplClientRun
```

## 4. `/logs` route not registered in PAPPL 1.4 even with `_WEB_LOG`

Setting `PAPPL_SOPTIONS_WEB_LOG` *should* register a `/logs` route
that surfaces the log file in the web UI. In 1.4.10 the bring-up log
shows the standard `/`, `/cancelall`, `/config`, `/jobs`, `/media`,
`/printing` routes registered, but **no `/logs`** — and direct GET
to `http://<host>:8000/logs` 404s. The Logs link is also absent from
the printer page navigation.

Suspected cause: `papplSystemCreate`'s `logfile` argument was passed
as `"-"` (stderr). Stderr isn't seekable/readable, so the web log
viewer has nothing to render. Untested fix: pass a real file path
(`/tmp/hl5170dn-printer-app.log` for development,
`/var/log/hl5170dn-printer-app.log` for the systemd install). Will
verify in Phase 4 (Observability).

**Workaround that works today**: keep running the binary with
`./hl5170dn-printer-app server 2>&1 | tee runtime.log` and use
`tail -f runtime.log` from a second terminal. Functionally identical
to the web log viewer for debugging purposes.

## 5. Integer-last-digit truncation in PAPPL's logger (both 1.3 and 1.4)

PAPPL's `papplLog` formatter chops the last decimal digit of every
integer it prints. Examples seen on this build:

| Actual value | What the log shows |
|---|---|
| `port 8000`               | `:800`               |
| `MAX_CLIENTS 32768`       | `up to 3276`         |
| `data->ppm = 21`          | `Driver reports ppm 2` |
| `data->x_resolution = 300`| `printer-resolution-default=30x30dpi` |

Confirmed present in both 1.3.1 and 1.4.10. **Cosmetic only** — the
internal values PAPPL uses are correct (text-test.pdf prints crisply
at 300 dpi, which wouldn't happen if PAPPL believed the printer was
30 dpi). But it makes log-reading misleading.

When debugging, treat PAPPL's own log lines (`I [...]` and `D [...]`
banners) as approximate and rely on driver-side `papplLogJob` lines
for ground truth. Our `rstartjob_cb` logs `start job: %ddpi, %u
bytes/line` — `%ddpi` shows the actual value PAPPL handed us via
`options->header.HWResolution[0]`, so that's the trustworthy
"what resolution is the engine working at" reading.

Worth filing upstream once we have a reduced repro. Out of scope for
shipping the driver.

## 6. PAPPL 1.3.1 → 1.4 API drifts the agent had to fix

Quick reference for anyone tempted to copy the 1.3.1 driver verbatim
into a 1.4 project (or vice versa). Encountered during the upgrade:

- `papplSystemCreate` 9th argument: `tls_only` boolean, must be
  passed in 1.3.1 (was missing from initial draft).
- `papplMainloop` argument list grew: needs explicit `usage_cb` and
  `cbdata` slots in 1.3.1.
- `papplSystemSetPrinterDrivers` 5th argument: `create_cb`. Driver
  registration must happen inside `system_cb` *before*
  `papplPrinterCreate`, because `papplMainloop` registers `driver_cb`
  *after* `system_cb` returns.
- `pappl_pr_driver_data_t` field shape:
  - `media[]` is `const char *` (PWG names), not `pappl_media_col_t[]`.
    Media defaults go in `media_default` and `media_ready[]`.
  - `source[]` and `type[]` are `const char *`, use direct assignment
    not `strncpy`.
  - `identify_actions` field is `identify_default` +
    `identify_supported` (2 fields).
  - `has_copies` field doesn't exist; remove.
- PAPPL 1.3.1 quirks that needed defensive code:
  - `_papplContentString(0)` and `_papplScalingString(0)` return NULL
    and PAPPL passes them to `strlcpy` without guarding → SIGSEGV.
    Always set `content_default`, `scaling_default`, `quality_default`
    to non-zero values.
  - `data->bin[data->bin_default]` is accessed without
    `num_bin > 0` guard. Always set `num_bin = 1`, `bin[0] =
    "face-down"`, `bin_default = 0`.

Fixed across commits `155318e` and `2594fe8`; full rationale lives
in the latter's commit message.

## 7. C escape-sequence and printf gotchas in PJL/PCL string literals

Two specific bugs that bit on first try:

- `"\x1bE"` is a single character `0x1BE` (out of range, compiler
  warning), not `0x1B` followed by `'E'`. Hex escapes consume any
  trailing hex digits. Use `"\033E"` or `"\x1b" "E"` (string
  concatenation forces the boundary).

- The Universal Exit Language sequence `"\x1b%-12345X"` contains a
  literal `%`. If you pass it through `snprintf`, the `%` is
  interpreted as a format specifier and the output is corrupted
  (and `-Wformat` warns). Write UEL directly to `papplDeviceWrite`;
  reserve `snprintf` for the variable PJL content lines that follow.

Both fixed in commit `2594fe8`.

## 8. papplJobCreatePrintOptions — argument count varies; third arg is speculative

From PAPPL issue #60 (the num_pages/duplex bug) we can confirm the function
takes at least `(job, num_pages)`.  Our call site uses
`papplJobCreatePrintOptions(job, (unsigned)INT_MAX, false)` with a third `bool`
argument; if the actual installed header only takes two arguments the compiler
will fail with "too many arguments to function".  In that case drop `false`:

```c
options = papplJobCreatePrintOptions(job, (unsigned)INT_MAX);
```

Always pass `INT_MAX` (not 0 or 1) for num_pages when you don't know the
page count — passing 0 or 1 causes PAPPL to override duplex-default to
single-sided (the root cause of issue #60).

## 9. papplSystemAddMIMEFilter — call site rules

`papplSystemAddMIMEFilter` must be called AFTER `papplSystemCreate()` and
BEFORE `papplSystemRun()` (i.e., while the system is not yet running). In
our architecture that means inside `system_cb`, after `papplPrinterCreate`.

Signature confirmed from PAPPL 1.4 docs:
```c
void papplSystemAddMIMEFilter(
    pappl_system_t        *system,
    const char            *srctype,   /* "application/pdf" */
    const char            *dsttype,   /* "image/pwg-raster" */
    pappl_mime_filter_cb_t cb,
    void                  *cbdata);
```

The `pappl_mime_filter_cb_t` is: `bool (*)(pappl_job_t *, pappl_device_t *, void *)`.

## 10. PDF filter design: drive raster callbacks directly, not PWG byte output

The PAPPL 1.4 docs say: "A raster filter that needs to print more than one
image must use the raster callback functions in the pappl_pr_driver_data_t
structure directly."

Interpretation: even with `dsttype = "image/pwg-raster"`, our filter does NOT
produce PWG raster bytes — it calls `rstartjob_cb` / `rstartpage_cb` /
`rwriteline_cb` / `rendpage_cb` / `rendjob_cb` from `pappl_pr_driver_data_t`,
obtained via `papplPrinterGetDriverData(papplJobGetPrinter(job), &drv)`.

The `device` parameter passed to the filter IS the physical device.  The
filter calls the raster callbacks with it directly; those write PCL to USB.
PAPPL does NOT call the raster callbacks again after the filter returns.

If this interpretation is wrong (i.e., if we see double-initialisation or the
printer gets two PJL headers) the fix is:
  a) Change dsttype to something non-raster like "application/vnd.hp-PCL".
  b) In the filter, write PCL bytes directly with papplDeviceWrite() instead
     of calling the raster callbacks.  This bypasses PJL wrapping but should
     still print if the printer accepts bare PCL.

## 11. PDF→PBM via gs pbmraw — verified format matches rwriteline_cb

`gs -sDEVICE=pbmraw` produces P4 (raw PBM): 1-bit, MSB-first, pixels packed
8 per byte, rows padded to byte boundaries.  Width in bytes = ceil(width/8).

This is exactly the format `hl5170dn_rwriteline` expects:
- `options->header.cupsBytesPerLine` = `(width + 7) / 8`
- `line` bytes: MSB = leftmost pixel

The packbits encoder then compresses these bytes for PCL `ESC *b <n> W` transfer.

Multi-page: gs outputs one P4 block per page, concatenated.  Each block has
its own "P4\n<w> <h>\n<data>" header.  The filter loops on fgets looking for
"P4" magic, reads dimensions, then fread()s the raw pixels directly.

Key: switch from fgets (text mode) to fread (binary mode) immediately after
parsing the PBM header.  fgets must NOT be used to read pixel data — it will
corrupt binary content that contains 0x0a (newline) bytes.


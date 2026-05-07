# PRD: Native PAPPL Driver for Brother HL-5170DN

## Context and framing

This is a learning project, not a production driver. The goal is to evaluate
whether agent-assisted coding (Claude Code) can produce a working, narrowly-scoped
native printer driver against an unfamiliar API surface (PAPPL) targeting
specific old hardware (Brother HL-5170DN, 2003) on a constrained host
(Raspberry Pi 3B+, USB-attached).

The PRD is being written knowing it will be published alongside the resulting
code as a reference for others considering similar "modern stack, old hardware"
projects. Honesty about scope and trade-offs takes priority over polish.

The existing pi-printer repo (https://github.com/jameshowison/pi-printer) is
the working baseline. It uses CUPS + a shell-script filter wrapping Ghostscript
ljet4 + hand-crafted PJL. This project replaces that with a single PAPPL-based
binary running as a systemd service.

## Goals

1. Print PDF, PWG Raster, Apple Raster, JPEG, and PNG from any IPP-Everywhere
   client (macOS Print dialog, iOS AirPrint, Linux CUPS) to a USB-attached
   HL-5170DN.
2. Match or exceed pi-printer on output quality at 300 dpi, 600 dpi, and HQ1200.
3. Expose Brother-specific options through standard IPP attributes: duplex
   long/short edge, toner save (econo mode), input tray selection, media type,
   resolution including HQ1200, copies.
4. Poll the printer over the USB back-channel via PJL `INFO` queries to populate
   `marker-supply-low-report` / `marker-levels` IPP attributes. Best-effort —
   if the printer doesn't respond, report unknown rather than fail.
5. PAPPL's web admin UI accessible at `http://pi.local:8000/`, including the
   live log viewer, with log level adjustable per-printer from the UI.
6. Run as a systemd service. No snap. No CUPS dependency at runtime.
7. Silently substitute Letter for incoming A4/A5/A6/Legal/Executive jobs
   (with notification on the job and in logs), since the printer is
   permanently loaded with Letter and tray-mismatch pauses are the most
   common real-world annoyance. Loaded paper size configurable.
8. Build cleanly on Raspberry Pi OS (64-bit, current stable) from source with
   documented dependencies.

## Non-goals

- Supporting any printer other than HL-5170DN. Generalising to "all Brother
  PCL5e lasers" is explicitly out of scope; if it happens to work on an
  HL-5070N or HL-1850 because the PCL surface is similar, fine, but we won't
  test or document it.
- Network-attached printer support. USB only. (The HL-5170DN has Ethernet, but
  using it would route around the interesting hard parts of this project.)
- PCL-XL / PCL6 output. PCL5e only. (See pi-printer's README — OpenPrinting's
  "PCL6" listing for this model means PCL 5e Standard, not PCL XL.)
- Color. The printer is monochrome.
- A snap package, deb package, or any distribution artifact beyond a built
  binary, a systemd unit file, and a README. Anyone running this builds it
  themselves.
- Long-term maintenance commitment. If PAPPL's API changes incompatibly in
  18 months, the response is "pin to the last working PAPPL version" not
  "track the API forever."

## Architecture

Single binary, `hl5170dn-printer-app`, linking PAPPL. Runs as a long-lived
process under systemd. Exposes IPP-Everywhere on a configurable port (default
8000). Talks to the printer via PAPPL's USB device API (`pappl_device_t`).

### Driver callbacks to implement

PAPPL's driver model is a struct of callbacks plus a data descriptor. The
callbacks for a raster-mode driver are roughly:

- **`printer_cb`** — driver setup. Populate `pappl_pr_driver_data_t` with
  resolutions (300, 600, HQ1200 mapped to 1200×600 in raster terms), media
  sizes (Letter, Legal, A4, A5, Executive, Envelope variants per pi-printer's
  PPD), source/tray names, type names, duplex modes, supported output bins.
  Declare PCL5e + PJL as the printer language by setting `*format` fields
  to indicate this is a `image/pwg-raster` consuming driver.

- **`rstartjob_cb`** — emit PJL Universal Exit Language sequence
  (`<ESC>%-12345X`), then `@PJL` line, then per-job PJL `SET` commands
  covering **all printer-control state**: `RESOLUTION`, `ECONOMODE`,
  `DUPLEX`, `BINDING` (only when duplex is on), `SOURCETRAY`, `MEDIATYPE`,
  `COPIES`, and `LPARM : PCL PAPER=<size>`. Mapping table is in pi-printer's
  `brother-hl5170dn-pjl` filter — copy it verbatim, including the exact
  PJL value names (`LETTER`, `MPTRAY`, `REGULAR`, `LONGEDGE`, etc.).
  End the header with `@PJL ENTER LANGUAGE=PCL`.

  **These PJL settings persist across the `<ESC>E` PCL resets emitted
  between pages and at job end.** This is the design pi-printer relies on,
  and it's documented in Brother's tech manual. Do not duplicate any of
  these settings as PCL5e commands in `rstartpage_cb` — the per-page PCL
  commands have lower precedence than PJL `LPARM:PCL` and will create
  hard-to-diagnose conflicts if the values disagree.

- **`rstartpage_cb`** — emit only raster-encoding setup. The exact sequence
  depends on resolution (see "Raster encoding and HQ1200" below). For
  300 and 600 dpi: `<ESC>E` reset, raster resolution `<ESC>*t<N>R`, raster
  presentation `<ESC>*r0F`, raster compression mode `<ESC>*b2M` (TIFF
  packbits), start raster `<ESC>*r1A`. **Do not** emit paper size, paper
  source, or duplex commands here — those are PJL's job.

- **`rwriteline_cb`** — called once per output line. Receives 8-bit grayscale
  (or 1-bit if we tell PAPPL we want it pre-halftoned — see "halftoning" below).
  Halftone if needed, compress, emit `<ESC>*b<count>W<data>`.

- **`rendpage_cb`** — `<ESC>*rC` end raster, formfeed `<0x0C>`, `<ESC>E` reset.

- **`rendjob_cb`** — `<ESC>%-12345X@PJL EOJ\r\n<ESC>%-12345X` to close the
  PJL job cleanly.

- **`status_cb`** — called periodically by PAPPL to refresh printer state.
  This is where supply-level polling goes (see "supply levels" below).

- **`identify_cb`** — implement as no-op (the HL-5170DN has no audible
  identify mechanism).

### Halftoning

Two options:

1. Tell PAPPL we want pre-halftoned 1-bit input by setting the driver's
   raster format to PWG/Apple Raster monochrome 1-bit. PAPPL's bundled raster
   pipeline does the halftoning for us using libcupsfilters' `cfOneBitLine()`.
   Gets us ordered-dither quality, which is fine for text and acceptable
   for photos at 600 dpi.

2. Take 8-bit input and halftone ourselves, optionally linking libgutenprint
   for its better dithering.

**Decision: start with option 1.** It's simpler, it removes a whole class of
bugs from the implementation, and the output quality difference for a
non-photo printer is small. Document this decision so it can be revisited if
photo quality ends up being a complaint.

### Raster encoding and HQ1200

This is the most printer-specific and least standard part of the
implementation. Brother's PCL technical manual (Chapter 2, §6.3.8 "Set
compression mode" and §6.3 "Horizontal 1200-dpi image format mode") is the
only authoritative source. Compression modes accepted by the HL-5170DN per
that manual are: **0, 1, 2, 3, 5, 9, 1152, 1024**.

Modes 0–5 are standard HP PCL5e (uncompressed, RLE, TIFF packbits, delta
row, adaptive). Modes 9, 1024, and 1152 are Brother extensions:

- **Mode 9** is described in the manual as repeated replacement data — an
  extended delta-row variant.
- **Modes 1024 and 1152** are tied to "Raster Graphic Mode 1027," Brother's
  Horizontal 1200-dpi image format. They are the *only* way to transmit
  actual HQ1200 raster to this printer.

**For 300 and 600 dpi:** use mode 2 (TIFF packbits). It's standard, well
understood, simple to implement, and good enough on a Pi 3B+ where
encoding speed matters more than output bytes saved. Mode 3 (delta row) is
worth considering later if USB bandwidth or Pi CPU becomes the bottleneck
on long jobs, but the savings only materialise on pages with lots of
vertical redundancy (text), and packbits is already tight on those.

**For HQ1200: open question.** The pi-printer baseline declares HQ1200 in
its PPD and sets `@PJL SET RESOLUTION=1200`, then lets Ghostscript ljet4
emit the raster. **ljet4 is a generic HP LaserJet 4 driver from the 1990s
and almost certainly does not know about Brother's Mode 1027 / mode
1024-1152 extensions.** It will emit 600 dpi raster regardless of what
PJL says. Whether the HL-5170DN's engine then delivers actual 2400×600
HQ1200 output by upsampling, or just prints at 600 dpi with HQ1200
flagged as "on" in firmware, is unclear without inspecting the byte stream
pi-printer produces and comparing physical output. **This driver project
should resolve that question first**, before implementing HQ1200 — the
test is "capture pi-printer's USB byte stream at the 'HQ1200' setting,
inspect for any `<ESC>*r1027` or mode 1024/1152 sequences." If absent,
pi-printer's HQ1200 is nominal; matching it requires no special work.

If we want *actual* HQ1200 (2400×600 raster), three sub-options:

1. **Skip it.** Declare 300 and 600 dpi only. Honest, simple, possibly
   matches pi-printer's real-world behaviour anyway.
2. **Claim it but emit standard 600 dpi raster** when selected. Matches
   pi-printer's apparent behaviour. Document the limitation.
3. **Implement Mode 1027 + Brother modes 1024/1152.** Requires reading
   the Brother manual carefully (pages 89–101 of the PCL chapter cover
   this), implementing whatever compression these modes use (the manual
   describes them but they're not in any public reference outside it),
   and testing against the printer. **This is the most interesting
   outcome for the meta-goal of evaluating agentic coding on weird old
   hardware** — it's exactly the case where the agent must extract
   protocol details from a vendor PDF with no internet fallback.

**Decision for the implementation phase:** start with option 1 (skip
HQ1200) to get a working driver, then attempt option 3 as a stretch goal.
If option 3 fails after focused effort, fall back to option 2 with
documentation. Capture and publish the byte-level investigation of what
pi-printer actually emits at the "HQ1200" setting either way — it's
useful reference material for anyone else looking at this printer.

### Media substitution

The HL-5170DN is permanently loaded with US Letter paper. The most common
real-world annoyance with the existing pi-printer setup is jobs from
clients that default to A4 (an iPhone with a region setting of UK, an
imported PDF, etc.) — the printer pauses with a tray-mismatch error and
requires either a physical front-panel button press to override, or
cancelling the job and resending. Pi-printer addresses this in its filter
with a `LOADED_PAPER` env var that coerces incoming page sizes to Letter
and lets Ghostscript's `-dPDFFitPage` rescale the content.

In the native PAPPL driver, the equivalent has to happen at the IPP
attribute layer rather than in a filter, because PAPPL — not our code —
runs the PDF→raster pipeline based on the resolved IPP attributes. By
the time `rwriteline_cb` is called, the page geometry is fixed.

**Implementation:** hook the job-creation path (PAPPL exposes this via
the printer driver's job-creation callback or by inspecting the job's
IPP attributes early in processing) and rewrite the `media` attribute
when it matches a coercible size:

| Incoming `media` | Action |
|---|---|
| `na_letter_8.5x11in` | pass through |
| `iso_a4_210x297mm` | rewrite to `na_letter_8.5x11in`, mark substituted |
| `iso_a5_148x210mm` | rewrite to `na_letter_8.5x11in`, mark substituted |
| `iso_a6_105x148mm` | rewrite to `na_letter_8.5x11in`, mark substituted |
| `na_legal_8.5x14in` | rewrite to `na_letter_8.5x11in`, mark substituted |
| `na_executive_7.25x10.5in` | rewrite to `na_letter_8.5x11in`, mark substituted |
| Envelope sizes (DL, C5, Com10, Monarch, ISOB5) | pass through unchanged |

Envelopes are explicitly *not* coerced — they require manual loading
into the MP tray, and silently swapping them for Letter would waste paper
and produce wrong output. If the user requests an envelope size they
either know what they're doing or the job will pause for manual feed,
which is the correct behaviour.

After the rewrite, PAPPL runs Ghostscript with the resolved (Letter)
page size. Ghostscript's `-dPDFFitPage` (which PAPPL passes by default
when invoking gs for raster generation) scales the original A4 PDF
content to fit Letter without clipping. Same mechanism pi-printer relies
on, just invoked one layer up.

**Notification.** When a substitution happens, surface it through three
channels in increasing visibility:

1. **Log at Info level** in PAPPL's web UI log: `Job 42: substituted Letter for A4 (loaded paper)`. Visible in the live log viewer at
   `http://pi.local:8000/`. Lowest friction to implement, but requires
   actively looking.
2. **`job-state-message`** set to `Substituted Letter for requested A4`. Some clients (notably iOS) display this in the print queue UI; macOS
   typically does not. Free; set it always when substituting.
3. **`job-state-reasons`** add a custom value like `media-substituted-warning`. IPP allows custom values prefixed with
   the vendor name; PAPPL passes them through. Visible to anyone using
   `ipptool` or `lpstat -W`, but invisible in normal client UIs.

**Strict mode (alternative behaviour).** A PAPPL vendor option
`media-mismatch-action` with values `substitute` (default) and `reject`.
In `reject` mode, jobs requesting non-loaded sizes fail at job creation
with an IPP error and `job-state-message` of "Loaded paper is Letter;
requested A4. Change client setting or reload printer." The job never
prints, the client surfaces the error in the print dialog or queue UI,
and no paper is wasted on a substituted print the user doesn't want.

The trade-off: `substitute` mode is forgiving (print succeeds, output is
usable) but can produce unwanted scaling on documents where layout
matters. `reject` mode is strict (no surprise output) but means every
A4-defaulting iPhone visitor hits a wall. Default to `substitute` for the
typical home-lab case where "just print it on whatever's loaded" is the
right answer 95% of the time; expose `reject` for users who'd rather
fail loudly.

**Configuration.** Loaded paper size is a PAPPL vendor option exposed
in the web UI under printer settings, not hardcoded. Default `Letter`,
also accepts `A4` for users who load A4. When the loaded size is `A4`,
the coercion table inverts (Letter, Legal, Executive coerce to A4; A5,
A6 coerce up to A4). Both directions use `-dPDFFitPage`, which scales
in either direction without clipping.

**Edge case: explicit user override.** If a user *really* wants an A4
job to print at A4 (e.g. they manually loaded A4), they can either
flip the loaded-paper vendor option in the web UI before submitting,
or PAPPL's standard `media-col-database` mechanism can be used to
request a non-coercible size. The simpler path is the vendor option;
document it in the README.

### Logging and observability

PAPPL's default log format prefixes job lines with `[Job N]` using its
internal job ID. The ID is correlatable but not human-readable —
"which print was Job 7?" requires cross-referencing with the web UI's
job list. For a home-lab printer that's mildly annoying; for debugging
a misbehaving job from a guest's phone it's worse.

**Driver-emitted log lines should include a richer prefix.** Construct
from the IPP attributes the job carries:

| Attribute | Notes |
|---|---|
| `job-name` | What most clients send as a label. macOS = filename. iOS Safari = page title. Some apps = app name. Some = `Untitled`. |
| `document-name` | Sometimes filename when `job-name` is something else. Not always populated. |
| `document-format` | MIME type. Useful for synthesising labels when name fields are empty (`<PDF>`, `<photo>`). |
| `job-originating-host-name` | Hostname or IP. PAPPL fills this from the connection — reliable, not client-claimed. |
| `requesting-user-name` | Username/device name claimed by client. iOS sends device name (`James's iPhone`). Untrusted but fine for a log label. |

Prefix construction:

1. Start with `Job N`.
2. Append `: <name>` where name is the first non-empty of
   `document-name`, `job-name`, or a synthesised label from
   `document-format` (`<PDF>`, `<photo>`, `<raster>`).
3. Append ` from <host>` where host is `job-originating-host-name`,
   falling back to `requesting-user-name`, falling back to nothing.
4. Truncate the whole prefix to 60 chars to keep log lines readable in
   the web UI.

Result:

```
[Job 7] State changed to processing.                                    ← PAPPL
[Job 7: report.pdf from Jameses-MacBook-Pro] Substituted Letter for A4. ← driver
[Job 7: report.pdf from Jameses-MacBook-Pro] PJL: RES=600 DUPLEX=ON     ← driver
[Job 7] State changed to completed.                                     ← PAPPL
```

The job ID `[Job 7]` appears in every line — PAPPL's terse state-change
lines stay grep-correlatable with the driver's richer ones. PAPPL's own
log format is not customisable from a driver; we make ours readable and
accept that PAPPL's lines stay terse.

**Sanitise inputs before logging.** `job-name` can contain anything —
newlines, control characters, very long strings, non-ASCII. Strip
control chars (replace with `?`), replace newlines with spaces, truncate
before adding to the prefix. Otherwise a job named with embedded newlines
can mangle log output or be used for log-injection-style spoofing.

**Where to use the rich prefix.** All driver-emitted log lines:
substitution events, PJL command summary at job start, supply polling
results, USB errors, page completions. Anywhere `papplLogJob()` is
called from driver code.

**Privacy note.** `job-originating-host-name` and `requesting-user-name` are personally identifiable. For a single-user home lab this is fine.
If the driver gets reused for a small-office shared printer, the log
captures who printed what — worth knowing before sharing log files.

### PJL command mapping

Copy verbatim from pi-printer's `brother-hl5170dn-pjl` filter and the Brother
tech manual checked into that repo. The exact strings matter — Brother's PJL
parser is strict about value names. Notable details:

- `RESOLUTION` takes a bare integer (`300`, `600`, `1200`), not `300dpi`.
- `BINDING` is only emitted when `DUPLEX=ON`.
- `LPARM : PCL PAPER=<value>` has spaces around the colon and is the
  PCL-language-scoped form (per Brother manual Ch5 §2). Plain
  `@PJL SET PAPER=` won't work the same way.
- Paper, tray, and media values are uppercase and Brother-specific
  (`LETTER`, `A4`, `MPTRAY`, `TRAY1`, `REGULAR`, `THICK2`, etc.).

| IPP attribute | PJL command |
|---|---|
| `print-quality=3` (draft) | `@PJL SET ECONOMODE=ON` + `@PJL SET RESOLUTION=300` |
| `print-quality=4` (normal) | `@PJL SET ECONOMODE=OFF` + `@PJL SET RESOLUTION=600` |
| `print-quality=5` (high) | `@PJL SET ECONOMODE=OFF` + `@PJL SET RESOLUTION=1200` *(see "Raster encoding and HQ1200"; PJL setting alone does not guarantee 2400×600 output)* |
| `sides=two-sided-long-edge` | `@PJL SET DUPLEX=ON` + `@PJL SET BINDING=LONGEDGE` |
| `sides=two-sided-short-edge` | `@PJL SET DUPLEX=ON` + `@PJL SET BINDING=SHORTEDGE` |
| `sides=one-sided` | `@PJL SET DUPLEX=OFF` |
| `media=stationery` | `@PJL SET MEDIATYPE=REGULAR` |
| `media=stationery-heavyweight` | `@PJL SET MEDIATYPE=THICK` |
| `media=…` (other types) | per pi-printer's mapping table |
| `media-source=tray-1` | `@PJL SET SOURCETRAY=TRAY1` |
| `media-source=by-pass-tray` | `@PJL SET SOURCETRAY=MPTRAY` |
| `media-source=auto` | `@PJL SET SOURCETRAY=AUTO` |
| `media (size) = letter` | `@PJL SET LPARM : PCL PAPER=LETTER` |
| `media (size) = a4` | `@PJL SET LPARM : PCL PAPER=A4` |
| `copies=N` | `@PJL SET COPIES=N` |

### Supply level polling

After job end (in `rendjob_cb` or on a timer in `status_cb`), send:

```
<ESC>%-12345X@PJL INFO STATUS
@PJL INFO PAGECOUNT
<ESC>%-12345X
```

Read response from the USB back-channel via `papplDeviceRead()` with a
timeout (500ms is reasonable). Parse for toner level if the printer reports
it. The HL-5170DN's PJL `INFO STATUS` response format is documented in
Brother's tech manual (in pi-printer repo as `Tech_Manual_Ch5_PJL.pdf`).

**Honest expectation:** the HL-5170DN may report only "ready" / "low toner"
/ "out of toner" as discrete states rather than a percentage. If so, map
those to `marker-levels` of 75 / 10 / 0 respectively and document the
approximation. If the printer doesn't respond at all over USB to PJL INFO
queries (a real possibility on a 2003 printer), document that too and fall
back to reporting `marker-levels=-2` (unknown).

This is the most likely-to-fail part of the project. **If polling doesn't
work after one focused day of effort, ship without it** and document the
attempt in the README. The rest of the driver is more important than this
single feature.

### USB device handling

PAPPL's USB device implementation handles the libusb plumbing. We open with
`papplDeviceOpen()`, read/write via `papplDeviceRead()` / `papplDeviceWrite()`.
The HL-5170DN's USB IDs are vendor 0x04f9 (Brother) — product ID can be
discovered with `lsusb` against the actual unit and hardcoded as a quirk if
PAPPL's auto-discovery doesn't pick it up.

## Build, packaging, deployment

- Single `Makefile` (no autotools). Builds against system PAPPL and
  libcupsfilters via pkg-config.
- Documented dependencies: `libpappl-dev`, `libcupsfilters-dev`, `ghostscript`
  (PAPPL invokes it for PDF rasterisation), `libusb-1.0-0-dev`. Versions
  pinned to whatever ships on current Raspberry Pi OS stable; if the version
  there is too old for current PAPPL, document the manual build steps for
  PAPPL itself.
- Systemd unit file installed to `/etc/systemd/system/hl5170dn-printer-app.service`.
  Runs as a dedicated `printapp` user with USB device group membership.
- No snap. No deb. `make install` puts the binary in `/usr/local/bin` and
  the unit file in `/etc/systemd/system`.

## Testing

The test suite is "print these documents and inspect the output." Specifically:

1. `text-test.pdf` and `image-test.pdf` from pi-printer repo. Print at 300,
   600, and HQ1200. Compare against pi-printer's output for the same documents.
2. Photo from iPhone via AirPrint. Print at default quality. Confirm one
   rasterisation (PWG → PCL, no PDF detour) by adding debug logging in
   `rwriteline_cb`.
3. Multi-page PDF with mixed text and images. Confirm duplex works in both
   long-edge and short-edge.
4. Job cancellation mid-print. Confirm PAPPL aborts cleanly and the printer
   doesn't get stuck in a bad PJL state.
5. Supply level: confirm web UI shows *something* sensible (a level, or
   "unknown" — not a crash).
6. Media substitution (default `substitute` mode): submit an A4 PDF from a
   Mac (or `lp -o media=a4`). Confirm: (a) the job prints on Letter without
   a tray-mismatch pause, (b) the content is scaled to fit Letter (no
   clipping at the bottom), (c) the web UI log shows the substitution,
   (d) `lpstat -W` or `ipptool` shows the `job-state-reasons` value.
   Repeat with an envelope size to confirm envelopes are *not* coerced.
7. Media substitution (`reject` mode): switch the vendor option to
   `reject`, submit an A4 PDF. Confirm: (a) the job fails before printing,
   (b) the client surfaces the error message, (c) no paper comes out.

No automated test suite. The output is bytes-on-paper; automating that is
out of scope. Manual smoke-test before each release, document the test
pass in commit messages.

## Risks and how we'll know we hit them

- **PCL5e encoding bugs.** Symptom: blank pages, garbled output, printer
  errors. Mitigation: capture the byte stream with `papplDeviceWrite()` tee'd
  to a file, compare against pi-printer's output for the same input. The
  pi-printer reference is the ground truth.
- **PAPPL API mismatch.** Symptom: build failures, runtime crashes in
  callbacks. Mitigation: pin to a specific PAPPL version, document it.
- **USB back-channel doesn't work.** Symptom: supply level polling hangs or
  returns garbage. Mitigation: timeout aggressively, fall back to "unknown,"
  document in README.
- **HQ1200 raster encoding is non-standard.** Brother's mode 1024/1152
  raster compression is documented only in their manual and not in any
  public PCL reference. If the agent has to implement it, it's working
  from a single source with no error-checking against other
  implementations. Symptom: HQ1200 jobs print blank, garbled, or at
  600 dpi anyway. Mitigation: see "Raster encoding and HQ1200" — start
  by skipping HQ1200, ship the driver, then attempt Mode 1027 as a
  stretch goal. Publish the byte-level investigation either way.
- **Media coercion happens at the wrong layer.** If the IPP-attribute
  rewrite isn't properly hooked, PAPPL may set up its raster pipeline
  for A4 before our coercion takes effect, leading to A4-sized raster
  being emitted with PJL claiming Letter — exactly the tray-mismatch
  pause the feature is meant to prevent. Symptom: tray-mismatch error
  on the printer despite the substitution log entry. Mitigation: verify
  with `papplDeviceWrite()` capture that the raster header bytes show
  Letter dimensions before claiming the feature works.
- **Halftoning quality complaints.** See "halftoning" — start with option 1,
  revisit if needed.

## What "done" looks like

- `make && sudo make install && sudo systemctl enable --now hl5170dn-printer-app`
  on a fresh Pi OS install produces a working IPP-Everywhere printer.
- Mac Preview can print a PDF to it via Bonjour discovery.
- iPhone AirPrint can print a photo to it.
- Web UI at `http://pi.local:8000` shows logs and lets you set log level.
- Supply level field is populated (with whatever fidelity we managed to
  achieve, including "unknown") and doesn't crash.
- README documents the full setup, the design decisions, the limitations,
  and the comparison to pi-printer.
- The agent-coding journal (separate from the README) documents which parts
  Claude Code got right first try, which parts needed iteration, and which
  parts had to be done by hand.

## Open questions for the implementation phase

- **What does pi-printer actually emit at HQ1200?** Capture the USB byte
  stream from a pi-printer setup with `cupsPrintQuality=High` and inspect
  for Brother Mode 1027 / mode 1024/1152 sequences. This determines
  whether HQ1200 in pi-printer is real or nominal, and whether the native
  driver needs to implement the Brother extensions or can just match
  pi-printer's standard-PCL5e output. **Do this on day one** — it
  shapes the rest of the raster encoding work.
- Does PAPPL's bundled Ghostscript on Pi OS handle 600 dpi raster
  generation fast enough? Measure on day one before committing to the
  architecture.
- Does `papplDeviceRead()` actually deliver back-channel data from a USB
  printer reliably, or do we need to drop to libusb directly? Find out by
  trying first.
- What's the right default port — 8000 (matches gutenprint-printer-app) or
  something distinctive? Probably 8000 with override in the systemd unit.

## What this project is testing about agent coding

The interesting question this project answers is not "can a working
HL-5170DN driver be written" — pi-printer already does that adequately. It's:

- Can an agentic coding loop navigate an unfamiliar C API (PAPPL) with
  thin documentation and learn from the source of comparable drivers
  (hp-printer-app, lprint)?
- Can it correctly synthesise hardware-specific knowledge from a vendor
  PDF (Brother's PJL manual) without hallucinating commands?
- Can it produce a PCL byte stream that matches a known-good reference
  (pi-printer's Ghostscript output) closely enough to print?
- Where does it need human intervention — debugging actual printer output,
  USB protocol quirks, performance tuning?

The PRD and resulting code, including the journal of agent interactions,
will be published together so others can calibrate their own expectations.

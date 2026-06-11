# pi-printer: project journey

How the Brother HL-5170DN PAPPL driver got from "CUPS + shell-filter +
Ghostscript ljet4" to a single native PAPPL binary that prints PDF, PWG raster,
JPEG, and PNG over USB with real-time per-impression progress. This document
consolidates the original PRD, intermediate plans, and the halftoning
retrospective into one narrative.

The Brother PCL and PJL chapter manuals (`Tech_Manual_Ch2_PCL.md`,
`Tech_Manual_Ch5_PJL.md`) remain alongside this file — they are the
authoritative protocol reference and are needed for ongoing work.

---

## 1. Starting point and goal

The pre-existing pi-printer repo used CUPS + a shell-script filter wrapping
Ghostscript `ljet4` + hand-crafted PJL. The goal of this project was to
replace that with a single PAPPL-based binary running as a systemd service,
exposing IPP-Everywhere directly (no CUPS at runtime) and talking PCL5e + PJL
to a USB-attached HL-5170DN.

The meta-goal was to evaluate whether agentic coding could navigate:

- An unfamiliar C API (PAPPL) with thin public docs
- A vendor PDF (Brother's PCL/PJL manual) as the only authoritative source
  for protocol details
- Old hardware (2003 monochrome laser) on a constrained host (Pi 3B+, USB)

The PRD's "done" criteria were: Mac/iOS print over Bonjour, web UI exposes
logs + supply level, builds cleanly from source on current Pi OS stable,
runs as a systemd service.

---

## 2. The phased build (Phases 0–7)

The original PRD broke the work into phases that survived more or less intact:

- **Phase 0** — investigations: confirm `papplDeviceRead` delivers
  back-channel data, measure GS performance, decide raster vs PCL XL.
  Outcome: dropped PCL XL in favor of PCL 5e per the HL-5070N tech manual
  (commit `7be63fc`).
- **Phase 1** — skeleton driver compiling against PAPPL 1.3.1, USB udev
  rule, first prints.
- **Phase 2** — PDF→PWG filter wired via `papplSystemAddMIMEFilter`, GS
  invoked in `pdf_filter_cb`, streaming grayscale pipeline.
- **Phase 3** — media substitution (Letter↔A4) with `substitute` and
  `reject` modes.
- **Phase 4** — rich job-prefix logging (`Job N: name from host`).
- **Phase 5** — PJL `INFO STATUS` / `INFO PAGECOUNT` polling in `status_cb`.
- **Phase 6A** — APT (Mode 1024) photo path; later disabled (`df8e38a`).
- **Phase 7** — packaging: README, systemd unit, web supplies page,
  PAGECOUNT-based supply baselines and reset tooling.

Verification was always manual: ipptool from the Pi against PAPPL on
`localhost:8000`, journalctl with `-o cat` to skip the double-timestamp
noise. CUPS clients (`lp`, `lpr`) were ruled out hard — they hit `/printers/`
not `/ipp/print`, and even when proxied they rasterize PDF to URF, bypassing
`pdf_filter_cb` and silencing the APT and resolution paths.

---

## 3. Halftoning: the most expensive mistake

The single longest detour was the **blue-noise halftoning regression**. Full
post-mortem follows because it is the most useful artifact of the project
for anyone considering "let the agent improve this algorithm" prompts.

### Timeline

| Commit | Date | What happened |
|---|---|---|
| (prior) | — | `threshold_row` (300 dpi) + `dither_row` 8×8 **clustered-dot** ordered dither (600 dpi). Correct for a xerographic laser. |
| `eedfdeb` | 2026-05-14 14:09 | Replaced both with `dither_row_bn`: 16×16 **blue noise** matrix from hp-printer-app + gamma=0.4545 baked into thresholds. |
| `f660782` | 2026-05-14 14:18 | Tuned gamma to 0.55, rationale "accounts for HL-5170DN dot gain". |
| 14 commits / 8 days | — | Bug invisible — only `text-test.pdf` was being tested, and text is solid black. |
| 2026-05-22 | — | Softball scoresheet printed with shaded column headers completely missing. |
| `9763297` | 2026-05-22 | Switched `pdf_filter_cb` from `pgmraw` to `pbmraw` — let GS halftone internally. |

### Why blue noise failed

The scoresheet had fills at grayscale ~0xe7–0xf0 (91–94% white). With
gamma=0.55, that produces ~1.6% black coverage.

- **Blue noise (dispersed-dot)** scatters those few black pixels as isolated
  single dots. On xerographic toner, isolated dots at low density do not
  fuse — they essentially disappear.
- **Clustered-dot AM screening** (what GS does) concentrates the same 1.6%
  coverage into halftone cells that *do* fuse, and the eye reads them as
  visible gray.

This is a physical property of toner fusion, not a calibration problem. The
gamma tuning gave the change a false air of domain awareness; in reality the
dispersed-dot failure mode at low coverage was unfixable by any tuning
constant.

### What an agent should have done differently

- **Defend the existing code before replacing it.** The 8×8 clustered-dot
  matrix was the right answer for this engine. Asking "what's wrong with the
  current implementation?" would have surfaced no concrete defect.
- **Test the document classes that depend on the change.** A single
  gray-fill page would have caught it on day one.
- **Treat "more sophisticated" references with suspicion across domains.**
  Blue noise from hp-printer-app is credible for *some* HP devices but not
  necessarily transferable to HL-5170DN's toner/paper combination.

### What replaced it

The PDF path now uses `gs -sDEVICE=pbmraw` — GS does the halftoning at
1-bit out, and the driver just packbits-encodes and emits PCL rows.
Subsequent work (`2b0b8ca`, `7edac3b`) routed the raster path (PWG/URF/JPEG
from AirPrint) through the same GS pbmraw pipeline by buffering 8-bit rows
to a temp PGM and running GS in `rendpage_cb`. The handwritten halftoning
code is gone; everything now goes through one GS halftoner.

---

## 4. Raster path: the "blank paper" debug

After the PDF path was working, AirPrint photos went through `rwriteline_cb`
→ PGM file → PostScript wrapper → GS pbmraw → PCL — and printed blank paper
despite logs showing all 6364/6364 rows written.

Hypotheses considered:

- **A**: GS outputs all-white PBM despite correct PS input
- **B**: PGM file content is actually white (skip logic broken)
- **C**: PackBits encoder collapses all-white rows incorrectly
- **D**: `fread` misalignment after PBM header parsing

Resolution (`7edac3b`): the PostScript `image` operator's CTM was wrong.
The original matrix used pixel dimensions as point dimensions, rendering
only ~12% of the image. The fix supplied `[w_pts 0 0 -h_pts 0 h_pts]` with
points correctly computed from `dimensions × 72 / resolution`.

The plan doc that drove that debug (preserving intermediate files,
synthetic-PGM tests, first-row PBM dumps) was a useful template — the
single most productive technique was preserving the temp files and
running GS manually outside the driver.

---

## 5. Real-time status sync

After the print pipeline stabilized, the next problem was that **macOS
Print Center lied about progress**. Three coupled issues:

1. **GS races ahead of the printer.** Raster pipes in faster than the
   printer prints, so the IPP "completed" state arrived 3 minutes before
   the last sheet emerged.
2. **No physical-page tracking** — only counted pages we *sent*.
3. **Idle status was stale.** Commit `d3107ee` had moved PJL polling to
   job-boundary only (to stop waking the fuser on every macOS
   Get-Printer-Attributes), which fixed wakeup but introduced 3-minute lag
   for paper-out detection.

### Research that fed the solution

- HL-5170DN supports `@PJL USTATUS PAGE = ON` — printer pushes
  `@PJL USTATUS PAGE\r\n<N>\r\n\f` on **physical** page completion.
- USTATUS settings persist across EOJ; only the PAGE counter resets per job.
- PAPPL rate-limits `status_cb` to 1/sec and skips it during jobs.
- `papplDeviceRead` has a 10-second libusb timeout — too long for
  mid-stream draining.
- No other PAPPL driver (hp-printer-app, lprint) does USB+PJL real-time;
  they use SNMP. The pattern was invented here.

### What shipped (`8e92120`)

- `@PJL USTATUS PAGE = ON` added to job header.
- Driver parses `@PJL USTATUS PAGE` packets distinctly from DEVICE/TIMED.
- Each PAGE event calls `papplJobSetImpressionsCompleted(job, N)` →
  macOS shows real "X of N".
- Back-channel draining between pages (50ms short timeout during stream,
  500ms during tail-wait — the streaming loop holds the device, so a long
  read would stall the next page's data flow).
- Tail-wait loop in `pdf_filter_cb` after streaming completes: keeps the
  IPP job in `processing` state until impressions == pagenum, or
  cancel, or no-progress timeout.

### The duplexer-flush bug (current as of `fb35eb0`)

Once tail-wait was in place, odd-page duplex jobs always timed out at the
60s no-progress threshold and the final sheet only ejected afterwards.

The HL-5170 holds the last simplex sheet inside the duplexer waiting for
either a back-side page or `@PJL EOJ`. EOJ was sent in `rendjob_cb`, which
PAPPL only invokes *after* `pdf_filter_cb` returns. So:

1. Render all pages → printer holds last sheet
2. tail-wait (waiting for a PAGE event that will never come)
3. Bail at 60s
4. `rendjob_cb` finally sends UEL + `@PJL EOJ`
5. Sheet finally ejects

Fix: split `pjl_write_job_trailer` into `pjl_write_job_eoj` (UEL + EOJ,
USTATUS still on) called *before* tail-wait, and `pjl_write_job_close`
(USTATUSOFF + powersave + final UEL) called from `rendjob_cb`. Tail-wait
no-progress timeout dropped from 60s → 20s, since once EOJ is sent any
silence longer than that is a real fault.

### Phases not yet shipped

- **Phase 3 — GS throttle.** Add backpressure so GS pace tracks printer
  pace. Block reading the next page from GS until
  `pages_sent - pages_printed < 2`. Requires a short-timeout USB read
  primitive (the default 10s `papplDeviceRead` is too coarse).
- **Phase 4 — cancel propagation.** Replace `popen` with `pipe/fork/execvp`
  so the driver has the GS PID. On `papplJobIsCanceled`, SIGTERM GS.
  Printer-side buffered PCL can't be aborted (UEL only flushes PJL state),
  but at least we stop adding to it.
- **Phase 6 Tier 2 — timed idle refresh.** `status_cb` does a single PJL
  INFO STATUS at most once per 60s when the device is otherwise idle.
  Bounded ≤ 60 wakeups/hour, way better than the pre-`d3107ee` rate.
- **Phase 6 Tier 3 — event-driven idle.** Leave the USB handle open
  between jobs, drop `USTATUSOFF` from the trailer, drain in `status_cb`.
  Gated on measuring whether a read-only USB session actually keeps the
  fuser awake.

---

## 6. Smaller corrections worth remembering

- **PCL `ESC &l<N>S` for duplex** (`7ff5f8b`, `79191d8`): PJL `DUPLEX=ON`
  alone was insufficient — needed the PCL duplex command, and it had to be
  emitted *before* `ESC E` so the reset didn't drop it.
- **Inter-page `ESC E`** (`58bc8a5`): removing the stray reset that was
  ejecting a held duplex sheet between pages.
- **Page-1 deferral** (`79191d8`): duplex setting applied only from page 2
  unless command ordering was correct.
- **HQ1200 / APT (Mode 1024)**: see §7 below. Implemented, tested,
  disabled. Reference code retained in `pdf_filter_cb` behind `if (0)`
  with re-enable notes.
- **TLS advertisement** (`ae2f57f`): macOS got stuck in a "printer is in
  use" loop when PAPPL advertised TLS over Bonjour. Disabled.
- **PAPPL state persistence** (`51b4ddc`, `8b2a0dc`): state must live in
  `/var/lib/hl5170dn-printer-app`, not `/tmp`, and must be applied via
  `printer_create_cb` not at first-job time, or reboot wipes config.
- **Supply page**: PAGECOUNT-based baselines persisted, reset tool at
  `/reset-supply`. `PAPPL_SUPPLY_TYPE_DRUM_IMAGING` doesn't exist — use
  `PAPPL_SUPPLY_TYPE_OPC` for the drum unit.

---

## 7. HQ1200 (Brother Mode 1024 / APT): implemented and abandoned

The PRD listed three options for HQ1200: skip it (Option 1), claim it but
emit standard 600 dpi (Option 2), or implement Brother's Mode 1027 /
1024 / 1152 from the manual (Option 3). The "most interesting outcome"
per the PRD was Option 3, and that's what was attempted under the Phase
6A "APT" label.

### What got built (commit `de2717d`)

From `Tech_Manual_Ch2_PCL` §6.3.8:

- `pjl_job_params_t` gained `bool apt`. When set, the PJL header emitted
  `@PJL SET APT=ON` + `@PJL SET IMAGEADAPT=ON` before
  `ENTER LANGUAGE=PCL`. Resolution forced to 600 so the printer could
  upscale from 150 dpi input.
- `apt_build_tiff_header()` constructed the exact 174-byte little-endian
  TIFF file header (12 tags, single strip, **BitsPerSample=8** — the
  APT trigger per the manual, XResolution=YResolution=150).
- `apt_render_pdf()` ran GS at 150 dpi (`pgmraw`), then for each page
  wrote PCL framing (`ESC*t600R ESC*b1024M ESC*r1A ESC*b<N>W`),
  streamed the 174-byte TIFF header, and piped GS pixel rows straight
  to `papplDeviceWrite` without buffering. The printer's APT engine
  did the halftoning internally and upscaled to its 1200 dpi raster.
- `pdf_filter_cb` branched on `IPP_QUALITY_HIGH` (and later
  `print-quality=5`) into the APT path; everything else stayed on
  the 600 dpi ordered-dither streaming path.

Validation gates included an offline `tiffinfo` check that the
synthesized header was structurally valid (T5, `d6c67a9`), and an
`apt-compare.sh` script that ran the same `chart-test.pdf` through
600 dpi / 150 dpi / APT for side-by-side prints.

### Why it was disabled (commit `df8e38a`)

Three independent findings made APT unattractive:

1. **Slower.** Photo content: 43s via APT vs 12s via 600 dpi standard
   path. The 150 dpi input is small, but the printer's internal
   halftoning + upscale step took longer than just streaming 600 dpi
   PCL.
2. **Marginally worse on photos.** The promised quality bump from
   printer-side halftoning didn't materialize against GS pbmraw at
   600 dpi.
3. **Worse on text and charts.** Per the Phase 6A decision gate
   (`f962416`): "600 dpi > 150 dpi > APT; APT text thickened and
   blurry." APT is specified for image content only, and even there
   GS halftoning at 600 was as good or better.

A secondary discovery: setting `printer-resolution=150dpi` from IPP
didn't actually drop GS to 150 dpi — PAPPL silently substituted 600
dpi back in. The 150 dpi rendering condition only happens inside the
APT path where the driver invokes GS with `-r150` directly.

### Outcome

`p->apt` is hardcoded `false`. The `pdf_filter_cb` branch is preserved
as `if (0) { … }` with a comment block describing how to re-enable
(set `apt=true` when `IPP_QUALITY_HIGH`, restore the branch). All
helper functions (`apt_build_tiff_header`, `apt_render_pdf`) remain
in `src/driver.c` — they compile, they work, they're just unreachable.

This is the cleanest answer to the PRD's open question. Brother Mode
1024 is real, the manual describes it accurately, and we successfully
drove the printer through it. The output isn't worth the latency
penalty or the content-type constraint. Anyone reviving it should
start with the existing reference code and the `apt-compare.sh`
harness, not from scratch.

---

## 8. What "done" looks like (status)

From the PRD's done-checklist:

- [x] `make && sudo make install && systemctl enable --now` produces a
      working IPP-Everywhere printer on fresh Pi OS.
- [x] Mac Preview prints PDF via Bonjour.
- [x] iPhone AirPrint prints photos.
- [x] Web UI at `http://tuttle-pi.local:8000` shows logs, supplies,
      memory time-series.
- [x] Supply level field populated from PAGECOUNT + baselines.
- [x] Real-time per-impression progress in macOS Print Center.
- [x] HQ1200 (Brother Mode 1024) — implemented per the manual, tested,
      and disabled after the apt-compare runs showed it slower and
      no better than 600 dpi. See §7. The PRD's stretch goal is
      complete; the answer is "not worth shipping."
- [ ] GS throttle, cancel propagation, idle-refresh tiers (Phase 3/4/6
      above) — designed, not implemented.

---

## 9. Lessons that generalize

- **Don't replace working algorithms without identifying what's wrong with
  them first.** The blue-noise regression cost 8 days because the existing
  clustered-dot dither had no concrete defect — it was replaced for being
  "less sophisticated."
- **A tuning constant on a wrong approach makes it harder to question, not
  easier to fix.** The gamma=0.55 in the blue-noise code looked like
  domain-aware engineering; it was just lipstick.
- **Test the document classes affected by the change, not just the easy
  case.** A 30-second gray-fill test page would have collapsed the
  detection latency from 8 days to minutes.
- **Use the vendor manual as the source of truth.** PJL value names
  (`LETTER`, `LONGEDGE`, `MPTRAY`) are case- and spelling-sensitive;
  general HP PCL knowledge gets you 80% there and then silently fails.
- **CUPS tooling will mislead you.** `lp`/`lpr`/`lpadmin` against a PAPPL
  endpoint either fails outright or rasterizes via CUPS first, bypassing
  the very code paths you're trying to test. ipptool only.
- **Preserve intermediate files when debugging multi-stage pipelines.**
  Half the raster-blank investigation became tractable the moment the
  temp PGM and PS files stopped getting unlinked.

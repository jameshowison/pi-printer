# Legacy CUPS-filter implementation

This directory contains the original Brother HL-5170DN driver
implementation: a CUPS shell-script filter wrapping Ghostscript `ljet4`
plus a hand-crafted PJL header, plus a PPD that points CUPS at the
filter.

It is **superseded** by the native PAPPL driver described in
[`../PRD-printer-applicance-rewrite-hl5170dn-pappl-driver.md`](../PRD-printer-applicance-rewrite-hl5170dn-pappl-driver.md)
and tracked in [`../plan.md`](../plan.md). It's kept here, working,
because:

1. The PAPPL rewrite uses it as ground truth for the PJL command
   mapping (`brother-hl5170dn-pjl` is the source the PRD says to copy
   "verbatim, including the exact PJL value names").
2. Phase 0 of the rewrite captures this filter's actual USB byte
   stream at `cupsPrintQuality=High` to determine whether the
   baseline's "HQ1200" is real (uses Brother's Mode 1027 / 1024 / 1152
   raster) or nominal (just `@PJL SET RESOLUTION=1200` over standard
   600 dpi raster). See
   [`../phase-0-investigations.md`](../phase-0-investigations.md).
3. The new driver may take longer than expected. Keeping the working
   one installable from this directory means there's a fallback while
   the rewrite is in flight.

## Files

| File | Role |
|---|---|
| `brother-hl5170dn-pjl` | The CUPS filter shell script. PJL header + Ghostscript invocation. |
| `Brother-HL5170DN-PCL.ppd` | CUPS PPD declaring filter routes for `application/vnd.hp-PCL`, PostScript, PDF, PWG raster. |
| `install.sh` | Installs filter + PPD into `/usr/lib/cups/filter` and `/usr/share/cups/model`, sets up the CUPS queue. Now uses `$(dirname "$0")` so it works whether invoked from this directory or the repo root. |

## Use

From the repo root:

```bash
bash legacy/install.sh
```

Or:

```bash
cd legacy && bash install.sh
```

Both work. The script resolves sibling files relative to its own
location.

## State as of the rewrite tag

Frozen reference point is the lightweight git tag
**`cups-filter-baseline`** (commit `4602cfc`, "quality options over
airprint"). Everything in this directory was working against a
USB-attached HL-5170DN on a Pi 3B+ at that commit, with the open
items listed in the previous plan.md (preserved at that tag) — most
notably the iOS-AirPrint-photo stall.

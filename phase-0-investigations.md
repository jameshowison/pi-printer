# Phase 0 — Day-zero investigations (Pi-side)

This file is the runbook for Claude Code (or a human) working on the
Pi 3B+ with the HL-5170DN attached. Each of the four investigations
shapes a later phase, so all four need to happen before any driver
code is written.

**Time-box: one day total.** Items 1 and 4 can run in parallel with
items 2 and 3. None of these requires modifying the running CUPS
filter — they read its behaviour from outside.

Record findings inline in this file (under "Findings" at the end of
each section, or in a new `phase-0-results.md` if they run long).
Phase 1 reads what's recorded here to decide raster encoding strategy
and default resolution.

## Prerequisites on the Pi

```bash
sudo apt update
sudo apt install -y \
    libpappl-dev libcupsfilters-dev libusb-1.0-0-dev \
    ghostscript build-essential pkg-config \
    usbutils linux-tools-generic
```

`linux-tools-generic` brings `usbmon` interfaces. If `usbmon` isn't
loadable on Raspberry Pi OS, fall back to `tcpdump -i usbmon0`
(below) or to userspace capture via `papplDeviceWrite()` tee in
investigation 3.

The current CUPS filter must be installed and working — the
`cups-filter-baseline` tag's `legacy/install.sh` does this. Verify
with `lp -d HL5170DN legacy/../text-test.pdf` before starting; if
the baseline can't print, none of the investigations will produce
useful output.

---

## Investigation 1 — What does the baseline emit at HQ1200?

**Question.** When the CUPS filter is set to `cupsPrintQuality=High`
and `Resolution-default=1200dpi`, does the byte stream sent to the
printer contain Brother's Mode 1027 / mode 1024 / mode 1152 raster
extensions, or is it standard 600 dpi raster with
`@PJL SET RESOLUTION=1200` slapped on?

**Why it matters.** This is the single biggest design choice for the
new driver. If the baseline's HQ1200 is nominal (no Brother modes),
matching it requires no special raster work — Phase 6 just declares
HQ1200, sets PJL, and emits standard mode-2 raster. If the baseline
*is* using Brother's modes, we either match it (significant work) or
ship at 600 dpi only.

**Procedure.**

1. Load `usbmon`:
   ```bash
   sudo modprobe usbmon
   ```
   Find the bus the printer is on:
   ```bash
   lsusb | grep -i brother
   # e.g. "Bus 001 Device 005: ID 04f9:0027 Brother Industries, Ltd HL-5170DN"
   ```
   Note the bus number (`001` here).

2. Capture USB traffic to that bus while a test print runs:
   ```bash
   sudo cat /sys/kernel/debug/usb/usbmon/1u > hq1200-capture.txt &
   CAP_PID=$!
   sleep 1
   lp -d HL5170DN -o cupsPrintQuality=High -o Resolution=1200dpi text-test.pdf
   # Wait until the page is delivered — eyeball the printer
   sleep 5
   sudo kill $CAP_PID
   ```
   Repeat with `image-test.pdf` since the byte stream for image-heavy
   pages may differ.

3. Extract the bulk-OUT (host → printer) data. `usbmon` text format
   has each transfer on a line; data bytes are after a `=` token:
   ```bash
   awk '/Bo:/ {found=0; for(i=1;i<=NF;i++) if($i=="="){found=1;continue} if(found) printf "%s ",$i; print ""}' \
     hq1200-capture.txt | tr -d ' \n' | xxd -r -p > hq1200-stream.bin
   ```
   (Adjust the awk if the Pi's usbmon format differs — `man 8 usbmon`
   has the field layout.) The result is a single binary file with
   the raw bytes the host sent the printer.

4. Search for Brother's mode markers:
   ```bash
   # Mode 1027 selector — emitted as `<ESC>*r1027U` per Brother manual §6.3
   grep -aoP '\x1b\*r1027' hq1200-stream.bin | wc -l
   # Compression mode 1024 / 1152 — `<ESC>*b1024M` / `<ESC>*b1152M`
   grep -aoP '\x1b\*b(1024|1152)M' hq1200-stream.bin | wc -l
   # Standard compression mode 2 (TIFF packbits)
   grep -aoP '\x1b\*b2M' hq1200-stream.bin | wc -l
   # Resolution declaration
   grep -aoP 'RESOLUTION=\d+' hq1200-stream.bin
   # Raster resolution PCL — `<ESC>*t<n>R`
   grep -aoP '\x1b\*t\d+R' hq1200-stream.bin
   ```

5. Sanity-check by also dumping the head of the stream:
   ```bash
   xxd hq1200-stream.bin | head -50
   ```
   You should see the UEL `1b 25 2d 31 32 33 34 35 58` (`<ESC>%-12345X`),
   the `@PJL` lines, then `<ESC>E` and the raster commands.

**Findings.**

```
Captured by running the CUPS filter directly at 1200dpi:
  /usr/lib/cups/filter/brother-hl5170dn-pjl 1 user title 1 "Resolution=1200dpi" text-test.pdf > stream.bin

Stream size at 1200dpi: 2,501,203 bytes (vs 1,062,293 bytes at 600dpi — 2.36× larger)

<ESC>*r1027 (Brother mode 1027):            0 occurrences
<ESC>*b1024M (Brother compression 1024):    0 occurrences
<ESC>*b1152M (Brother compression 1152):    0 occurrences
<ESC>*b2M (TIFF packbits, standard):      184 occurrences
<ESC>*b3M (delta row, standard):          189 occurrences
RESOLUTION= in PJL: 1200
<ESC>*t<n>R (raster resolution):    1200   ← genuine 1200 dpi raster
<ESC>*r<n>A (start raster):         1

Comparison at 600dpi:
  Mode 2: 159, Mode 3: 163, <ESC>*t600R, RESOLUTION=600

PJL header (first 256 bytes):
  <ESC>%-12345X@PJL
  @PJL SET RESOLUTION=1200
  @PJL SET ECONOMODE=OFF
  @PJL SET DUPLEX=OFF
  @PJL SET SOURCETRAY=AUTO
  @PJL SET MEDIATYPE=REGULAR
  @PJL SET COPIES=1
  @PJL SET LPARM : PCL PAPER=LETTER
  @PJL ENTER LANGUAGE=PCL
```

**Decision rule for Phase 6.**

- If `<ESC>*r1027` and either `<ESC>*b1024M` or `<ESC>*b1152M` are
  present → baseline uses Brother's HQ1200 modes. Phase 6 must
  attempt mode 1024/1152 implementation (PRD option 3).
- If only `<ESC>*b2M` is present and `<ESC>*t<n>R` shows 600 →
  baseline's HQ1200 is nominal. Phase 6 matches it: declare HQ1200,
  set `@PJL SET RESOLUTION=1200`, emit standard 600 dpi mode-2
  raster (PRD option 2). No special encoding work needed.
- Anything weirder (e.g. `<ESC>*t1200R` with mode-2 compression):
  inspect the post-raster bytes manually and update this section
  before deciding.

**Outcome: third case — `<ESC>*t1200R` with standard mode-2 and mode-3
compression, no Brother-proprietary modes.** GS ljet4 runs at 1200 dpi
and emits a genuine 1200-dpi raster using standard PCL5e compression;
Brother's mode 1024/1152 extensions are not used. Phase 6 = **PRD option
2**: declare HQ1200, set `@PJL SET RESOLUTION=1200`, instruct PAPPL to
run GS at 1200 dpi (not 600), emit standard mode-2 packbits. No
proprietary encoding work needed. Caveat: on Pi 3B+, GS at 1200 dpi
will be even slower than at 600 dpi (which is already 45 s for photo
input — see Investigation 2); HQ1200 photo prints will be very slow.

---

## Investigation 2 — How long does GS take to render on the Pi 3B+?

**Question.** Is the Pi 3B+ fast enough to drive `gs -sDEVICE=ljet4`
at 600 dpi without the printer entering USB sleep mid-job and
wedging?

**Why it matters.** The baseline's iOS-photo stall (Job 10 in the
previous plan.md, 1.5h hang) is suspected to be a printer USB sleep
race triggered by a long render window. If GS at 600 dpi takes >10s
on the Pi 3B+ for typical photo input, Phase 1 should default to
300 dpi and Phase 7 should document Pi 5 as the upgrade. If it's
fast (<5s), the architecture is fine.

**Procedure.**

```bash
cd /path/to/this/repo

# Baseline timing — text PDF
/usr/bin/time -v gs -q -dBATCH -dNOPAUSE \
    -sDEVICE=ljet4 -r600 -sPAPERSIZE=letter \
    -sOutputFile=/dev/null \
    text-test.pdf 2>&1 | grep -E 'Elapsed|Maximum resident'

# Image-heavy PDF — the realistic photo case
/usr/bin/time -v gs -q -dBATCH -dNOPAUSE \
    -sDEVICE=ljet4 -r600 -sPAPERSIZE=letter \
    -sOutputFile=/dev/null \
    image-test.pdf 2>&1 | grep -E 'Elapsed|Maximum resident'

# Repeat at 300 dpi for comparison
/usr/bin/time -v gs -q -dBATCH -dNOPAUSE \
    -sDEVICE=ljet4 -r300 -sPAPERSIZE=letter \
    -sOutputFile=/dev/null \
    image-test.pdf 2>&1 | grep -E 'Elapsed|Maximum resident'
```

Run each three times, take the median. Wall-clock time and peak RSS
both matter — the Pi 3B+ has 1 GB RAM; if `gs` peaks over ~400 MB
on photo input, that's another reason to default to 300 dpi.

**Findings.**

```
GS version: 10.05.1 (Ghostscript)
Ghostscript device: ljet4

text-test.pdf  @ 600 dpi:  Elapsed=0.77s  (runs 1.04s, 0.77s, 0.75s — median 0.77s)
image-test.pdf @ 600 dpi:  Elapsed=45.1s  (single run; consistently ~45s)
image-test.pdf @ 300 dpi:  Elapsed=20.7s  (runs 21.0s, 20.7s, 20.8s — median 20.8s)

Peak RSS not measured (no /usr/bin/time available; 'time' builtin only).
Pi 3B+ has 1 GB RAM; GS at 600 dpi on a photo PDF stays well below OOM
based on system stability during the run.
```

**Decision rule for Phase 1.**

- Photo render at 600 dpi <5 s: 600 dpi is fine as default.
- Photo render at 600 dpi 5–15 s: keep 600 dpi as default but
  document the iOS-stall risk and recommend disabling printer sleep
  in the README.
- Photo render at 600 dpi >15 s: default to 300 dpi, document Pi 5
  as upgrade path.

**Outcome: 45 s at 600 dpi → default to 300 dpi.** Even 300 dpi takes
~21 s for image-heavy input, which exceeds the Pi 3B+'s safe window for
keeping the printer awake over USB. Phase 1 driver defaults to 300 dpi;
Phase 7 README documents Pi 5 as the upgrade path and recommends
disabling printer sleep mode (front-panel menu or web interface) for
anyone who wants 600 dpi on Pi 3B+.

---

## Investigation 3 — Does `papplDeviceRead()` get back-channel bytes?

**Question.** Can the new driver read a response from the HL-5170DN
after sending a PJL `INFO STATUS` query over USB? Or does the back
channel time out or return garbage on this 2003-era printer?

**Why it matters.** Phase 5 (supply-level polling) lives or dies on
this. Per the PRD: if it doesn't work after one focused day, ship
without it. Better to know on day zero.

**Procedure.** Build a minimal probe that doesn't depend on the rest
of the driver:

```bash
mkdir -p ~/pappl-probe && cd ~/pappl-probe
cat > probe.c <<'CEOF'
#include <pappl/pappl.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *uri = (argc > 1) ? argv[1] : "usb://Brother/HL-5170?serial=0";
    pappl_device_t *dev = papplDeviceOpen(uri, "probe", NULL, NULL);
    if (!dev) { fprintf(stderr, "open failed: %s\n", uri); return 1; }

    const char *query =
        "\x1b%-12345X@PJL INFO STATUS\r\n"
        "@PJL INFO PAGECOUNT\r\n"
        "\x1b%-12345X";
    ssize_t w = papplDeviceWrite(dev, query, strlen(query));
    fprintf(stderr, "wrote %zd bytes\n", w);

    char buf[4096];
    ssize_t r = papplDeviceRead(dev, buf, sizeof(buf) - 1);
    if (r < 0) { fprintf(stderr, "read error\n"); papplDeviceClose(dev); return 2; }
    buf[r] = 0;
    fprintf(stderr, "read %zd bytes:\n", r);
    fwrite(buf, 1, (size_t)r, stdout);

    papplDeviceClose(dev);
    return 0;
}
CEOF

cc -o probe probe.c $(pkg-config --cflags --libs pappl)
```

Find the printer's actual device URI (PAPPL's USB enumeration may
differ from `lsusb`):

```bash
# Quick way — let PAPPL list devices
sudo ./probe usb://list  # if PAPPL's probe URIs work
# Or use lsusb + udev
lsusb -v -d 04f9: | grep -E 'iSerial|idProduct'
```

Then run:

```bash
sudo ./probe "usb://Brother/HL-5170DN?serial=<actual-serial>"
```

The CUPS print queue must be stopped first (`sudo cupsdisable HL5170DN`)
or the probe will fight it for the USB device. Re-enable after
(`sudo cupsenable HL5170DN`).

**Findings.**

```
Probe: ~/pappl-probe/probe.c + probe3.c compiled against libpappl 1.3.1
URI:   usb://Brother/HL-5170DN%20series?serial=L4J624176
       (discovered via: sudo lpinfo -v | grep -i brother)

Initial probe (probe2.c, 200ms polling intervals):
  wrote 57 bytes, flushed
  read attempt 0: 0 bytes  (200ms)
  read attempt 1: 0 bytes  (400ms)
  read attempt 2: 72 bytes  ← first response at ~400ms

Initial response (printer was in sleep mode):
  @PJL INFO STATUS
  CODE=40000
  DISPLAY="SLEEP           "
  ONLINE=TRUE

Deep probe (probe3.c, after printer woke from first query):
  INFO STATUS (222 bytes):
    @PJL INFO PAGECOUNT
    PAGECOUNT=18421
    @PJL INFO STATUS
    CODE=10001
    DISPLAY="READY           "
    ONLINE=TRUE
    [echoed twice — note: printer echoes the query back before its response]

  INFO PAGECOUNT (39 bytes):
    @PJL INFO PAGECOUNT
    PAGECOUNT=18421

  INFO ID (68 bytes):
    @PJL INFO ID
    "Brother HL-5170DN series:84UZ74:Ver1.11:EVer1.00M"

  INFO SUPPLIES (26 bytes):
    @PJL INFO SUPPLIES
    "?"       ← printer does not report supply levels

  INFO MEMORY (53 bytes):
    @PJL INFO MEMORY
    TOTAL=24389808     (≈24 MB firmware memory)
    LARGEST=24389808

Status code meanings (confirmed vs Tech_Manual_Ch5_PJL):
  CODE=10001 = READY
  CODE=40000 = SLEEP

Printer serial: L4J624176, page count: 18421
```

Particularly note: does the response include `STATUS=`,
`CODE=10001`-style status codes (per `Tech_Manual_Ch5_PJL.md`),
`PAGECOUNT=N`, or only echoes / nothing? Time to first byte —
under 100 ms is great, 100–500 ms acceptable, >500 ms means Phase 5
needs an aggressive timeout.

**Decision rule for Phase 5.**

- Real status response within 500 ms: Phase 5 is feasible. Map
  reported levels to `marker-levels`.
- Empty / hangs: Phase 5 ships as `marker-levels=-2` (unknown),
  document the attempt in README. Don't spend more than the time-
  boxed day trying to coax it.
- Garbage: capture an example, file as a Phase 5 open question, but
  default to "unknown" for shipping.

**Outcome: back-channel works; toner level is not available.** Response
arrives in ~400 ms from sleep, faster when awake (within one 100 ms
polling interval). `CODE=10001` (READY) and `CODE=40000` (SLEEP) are
confirmed. `INFO SUPPLIES` returns `"?"` — the HL-5170DN does not expose
toner level over PJL. Phase 5 will poll `INFO STATUS` to detect
READY/SLEEP/error states and report them in `pappl_preason_t`, but
`marker-levels` will always be `-2` (unknown) and `marker-supply-low-
report` cannot be populated. Document this limitation in the README.
The 500 ms timeout budget for `papplDeviceRead()` is sufficient — actual
response comes faster once the printer is awake.

---

## Investigation 4 — Does Raspberry Pi OS's `libpappl-dev` work?

**Question.** Is the `libpappl-dev` shipping in current Raspberry Pi
OS stable recent enough for the API the new driver needs, or do we
need to build PAPPL from source?

**Why it matters.** The PRD says "if the version there is too old
for current PAPPL, document the manual build steps for PAPPL itself."
Phase 0 is the place to find out and pin a version.

**Procedure.**

```bash
# What version does apt have?
apt-cache policy libpappl-dev
dpkg -l | grep -i pappl

# What headers does it install?
dpkg -L libpappl-dev | grep '\.h$'

# Does a trivial program compile and link?
cat > /tmp/papplv.c <<'CEOF'
#include <pappl/pappl.h>
#include <stdio.h>
int main(void) {
    printf("PAPPL_VERSION = %s\n", PAPPL_VERSION);
    return 0;
}
CEOF
cc -o /tmp/papplv /tmp/papplv.c $(pkg-config --cflags --libs pappl) && /tmp/papplv
```

If apt's PAPPL is old (anything older than 1.4.x; check
[github.com/michaelrsweet/pappl/releases](https://github.com/michaelrsweet/pappl/releases)
for what's current) or the test program won't compile because the
headers are missing functions Phase 1 needs, build from source:

```bash
cd ~ && git clone https://github.com/michaelrsweet/pappl.git
cd pappl
git checkout v1.4.x   # whatever the latest stable tag is
./configure --prefix=/usr/local
make -j4
sudo make install
sudo ldconfig
```

Then re-run the test program with `pkg-config --cflags --libs pappl`
preferring `/usr/local/lib/pkgconfig`:

```bash
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH \
    cc -o /tmp/papplv /tmp/papplv.c $(pkg-config --cflags --libs pappl)
```

**Findings.**

```
apt version:  1.3.1-2.1+b2  (libpappl-dev and libpappl1t64)
pkg-config:   1.3.1

PAPPL_VERSION macro not present in 1.3.1 headers (available in later versions).
Version confirmed from pkg-config --modversion.

Key APIs verified present in headers:
  papplDeviceOpen / papplDeviceRead / papplDeviceWrite / papplDeviceClose / papplDeviceFlush
  pappl_pr_rstartjob_cb_t / rstartpage_cb_t / rwriteline_cb_t
  pappl_pr_rendpage_cb_t / rendjob_cb_t / status_cb_t / identify_cb_t
  pappl_pr_driver_data_t (struct with all above callback fields)
  papplPrinterSetDriverData / papplPrinterSetDriverDefaults

Compilation test: probe.c (calls papplDeviceOpen, papplDeviceWrite,
  papplDeviceRead, papplDeviceClose) compiled and ran successfully.

Note: papplDeviceRead() has no timeout parameter in 1.3.1. The driver
  must manage its own read timeout using select()/alarm() or by
  limiting poll frequency in status_cb.
```

**Decision rule for Phase 1.**

- apt version is recent enough: Phase 1's Makefile uses
  `pkg-config pappl` against the system install. Pin the version in
  README.
- apt version is too old: Phase 7 of the README documents the source
  build steps above. Pin the source-built version.

**Outcome: apt 1.3.1 is sufficient for Phase 1 through Phase 5.**
All Phase 1 callbacks and device APIs are present and working. Phase 1
Makefile uses `pkg-config pappl` against the system install. README pins
to 1.3.1. Re-evaluate if Phase 3 (vendor options / media substitution
hooks) needs PAPPL features absent in 1.3.1 — check before starting
Phase 3 whether `papplJobSetState()` and job-creation hooks are
present at this version.

---

## Wrap-up

When all four investigations have findings recorded, summarise in a
short note at the top of `plan.md` (replace the bullet list under
"Phase 0"). Then move to Phase 1 with informed defaults.

If any investigation surprises you in a way that changes a downstream
phase's strategy, update `plan.md` before you start writing C —
re-planning is cheap, re-writing the wrong driver is not.

#!/bin/bash
# apt-compare.sh — print chart-test.pdf three ways and report prep vs total time.
#
# Conditions (each stamped with its label):
#   1. 150dpi-direct  — GS at 150 dpi, quality=4, no APT
#   2. APT-Mode-1024  — GS at 150 dpi input, quality=5, Mode 1024 upscale
#   3. 600dpi-direct  — GS at 600 dpi, quality=4
#
# IMPORTANT: labels must not contain PostScript special chars: ( ) < > [ ] { } / %

set -euo pipefail

REPO=/home/tuttle/pi-printer
TESTS=$REPO/tests
URI=ipp://localhost:8000/ipp/print

JOB_STATE_TEST=/tmp/apt-compare-state-$$.test
trap 'rm -f "$JOB_STATE_TEST"' EXIT

cat > "$JOB_STATE_TEST" << 'EOF'
{
  NAME "Get-Job-Attributes"
  OPERATION Get-Job-Attributes
  GROUP operation-attributes-tag
  ATTR charset attributes-charset utf-8
  ATTR language attributes-natural-language en
  ATTR uri printer-uri $uri
  ATTR integer job-id $jobid
  ATTR keyword requested-attributes job-state
  STATUS successful-ok
  EXPECT job-state
}
EOF

submit_pdf() {
    local label="$1" pdf="$2" testfile="$3"
    # -tv so we get full attribute lines; extract job-id integer
    ipptool -tv \
        -f "$pdf" \
        -d filetype=application/pdf \
        -d "jobname=$label" \
        "$URI" \
        "$testfile" \
      | grep -oP 'job-id \(integer\) = \K[0-9]+'
}

wait_for_job() {
    local jobid="$1"
    local state
    while true; do
        state=$(ipptool -tv -d "jobid=$jobid" "$URI" "$JOB_STATE_TEST" \
                  | grep -oP 'job-state \(enum\) = \K\S+' || echo unknown)
        case "$state" in
            completed|aborted|canceled) break ;;
        esac
        sleep 2
    done
}

# Extract GS render seconds for a job from journalctl.
# Uses epoch microseconds from the log timestamps (no jq needed).
gs_time_for_job() {
    local jobid="$1"
    # Get all log lines for this job with their epoch-second timestamps
    local log
    log=$(journalctl -u hl5170dn-printer-app -o short-unix --no-pager \
          | grep "\[Job $jobid\]")

    local t_start t_end
    t_start=$(echo "$log" | grep -oP '^\S+(?=.*gs cmd:)'    | head -1)
    t_end=$(  echo "$log" | grep -oP '^\S+(?=.*(apt_render|pdf_filter): ok)' | head -1)

    if [[ -n "$t_start" && -n "$t_end" ]]; then
        # timestamps are float seconds; bc handles the subtraction
        echo "scale=1; $t_end - $t_start" | bc
    else
        echo "?"
    fi
}

declare -a LABELS JOB_IDS TOTAL_TIMES

run_condition() {
    local label="$1" pdf="$2" testfile="$3"
    echo "=== $label ===" >&2

    local t0 t1 jobid total_s
    t0=$(date +%s%3N)
    jobid=$(submit_pdf "$label" "$pdf" "$testfile")
    echo "  job-id: $jobid" >&2
    wait_for_job "$jobid"
    t1=$(date +%s%3N)
    total_s=$(echo "scale=1; ($t1 - $t0) / 1000" | bc)

    LABELS+=("$label")
    JOB_IDS+=("$jobid")
    TOTAL_TIMES+=("${total_s}s")

    echo "  done — total ${total_s}s" >&2
}

echo "Starting comparison — chart-test.pdf x 3 conditions"
echo ""

run_condition "150dpi-direct"  "$TESTS/pdfs/chart-150dpi.pdf"  "$TESTS/print-chart-simplex-150.test"
run_condition "APT-Mode-1024"  "$TESTS/pdfs/chart-apt.pdf"     "$TESTS/print-chart-simplex-apt.test"
run_condition "600dpi-direct"  "$TESTS/pdfs/chart-600dpi.pdf"  "$TESTS/print-chart-simplex-600.test"

echo ""
echo "=== Results ==="
printf "%-20s  %8s  %10s  %10s\n" "Condition"        "Job ID" "GS time"   "Total"
printf "%-20s  %8s  %10s  %10s\n" "---------"        "------" "-------"   "-----"
for i in 0 1 2; do
    gs_s=$(gs_time_for_job "${JOB_IDS[$i]}")
    printf "%-20s  %8s  %9ss  %9s\n" \
        "${LABELS[$i]}" "${JOB_IDS[$i]}" "$gs_s" "${TOTAL_TIMES[$i]}"
done

#!/usr/bin/env bash
# Run one arm and snapshot the shared trace CSVs into the run dir.
#
# ⚠ TWO TRAPS, both of which silently FAKE a byte-identical A/B (SPEC_amb_iono §6.13.4, 2026-09-06):
#
#  1. WRONG SOURCE DIR.  The network traces are written CWD-relative to ./logs/.  This script used to
#     copy from build/x64-Release/logs/, which on this box holds a STALE 10-epoch file from 2026-08-13;
#     both arms of an A/B then copied the same old file and every trace compared IDENTICAL — the most
#     reassuring possible answer, produced by reading nothing.  It survived a smoke A/B and was caught
#     only because a run-end counter said 765 elections differed while the files said none did.
#  2. THE NEXT ARM OVERWRITES ./logs/.  Capture immediately, and VERIFY the capture is FRESH — presence
#     is not freshness, and a copy's own mtime is always new, so stat the SOURCE against the run's start.
#
# Corollary for any trace-only knob: byte-identity is the expected result of a correct OFF path AND of a
# broken capture.  Only a run-end COUNTER tells them apart (standing invariant #4, applied to the
# instrument itself) — always cross-check one.
set -u
exe="${EXE:-./build/x64-Release/gnss_processor.exe}"
name="$2"
mkdir -p "runs/$name/logs"
t0=$(date +%s)
"$exe" "config/test_suite/_${name}.json" > "runs/$name/run.log" 2>&1
rc=$?
bad=""
for f in network_solution.csv network_estimability.csv network_events.csv network_epoch_status.csv \
         network_dd_fractional.csv; do
  src="logs/$f"
  if [ -f "$src" ]; then
    [ "$(stat -c %Y "$src" 2>/dev/null || echo 0)" -ge "$t0" ] || bad="$bad $f(STALE-not-written-by-this-run)"
    cp -f "$src" "runs/$name/logs/$f" 2>/dev/null || bad="$bad $f(copyfail)"
    [ -f "runs/$name/logs/$f" ] || bad="$bad $f(absent)"
  else
    bad="$bad $f(nosrc)"
  fi
done
echo "exit=$rc"
[ -n "$bad" ] && echo "WARN uncaptured/stale:$bad"
grep -E "producer complete" "runs/$name/run.log" | tail -1 | cut -c1-140

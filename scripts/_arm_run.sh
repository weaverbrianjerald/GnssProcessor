#!/usr/bin/env bash
# Run one arm and snapshot the shared trace CSVs into the run dir.  The CSVs live in
# build/x64-Release/logs/ and are OVERWRITTEN by the next run, so an A/B must capture
# them immediately and verify the capture — a silently missed copy makes the grader
# read whatever run happened to write last.
set -u
name="$2"
mkdir -p "runs/$name/logs"
./build/x64-Release/gnss_processor_card1f.exe "config/test_suite/_${name}.json" > "runs/$name/run.log" 2>&1
rc=$?
missing=""
for f in network_solution.csv network_estimability.csv network_events.csv network_epoch_status.csv; do
  src="build/x64-Release/logs/$f"
  if [ -f "$src" ]; then
    cp -f "$src" "runs/$name/logs/$f" 2>/dev/null || missing="$missing $f(copyfail)"
    [ -f "runs/$name/logs/$f" ] || missing="$missing $f(absent)"
  else
    missing="$missing $f(nosrc)"
  fi
done
echo "exit=$rc"
[ -n "$missing" ] && echo "WARN uncaptured:$missing"
grep -E "producer complete" "runs/$name/run.log" | tail -1 | cut -c1-140

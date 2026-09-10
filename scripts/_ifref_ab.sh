#!/usr/bin/env bash
# Card-R ref-gate A/B: capture CWD-relative ./logs/ with SOURCE-mtime freshness assertion.
set -u
EXE=./build/x64-Release/gnss_processor_ifref.exe
for name in ifrefg_off ifrefg_on; do
  mkdir -p "runs/$name/logs"
  t0=$(date +%s)
  echo "=== $name start $(date +%H:%M:%S) ==="
  "$EXE" "config/test_suite/_${name}.json" > "runs/$name/run.log" 2>&1
  echo "exit=$? elapsed=$(( $(date +%s) - t0 ))s"
  bad=""
  for f in network_events.csv network_if_slip.csv network_solution.csv network_epoch_status.csv network_flag_rates.csv; do
    src="logs/$f"
    if [ -f "$src" ]; then
      m=$(stat -c %Y "$src" 2>/dev/null || echo 0)
      [ "$m" -ge "$t0" ] || bad="$bad $f(STALE)"
      cp -f "$src" "runs/$name/logs/$f" || bad="$bad $f(copyfail)"
    else bad="$bad $f(nosrc)"; fi
  done
  [ -n "$bad" ] && echo "WARN:$bad"
  echo "captured: $(ls -la runs/$name/logs/ | awk '{print $9\"=\"$5}' | tr '\n' ' ')"
done

#!/usr/bin/env bash
# §6.14.6 three-arm validation.  Captures CWD-relative ./logs/ with a SOURCE-mtime
# freshness assertion (the stale-trace-dir trap, SPEC_amb_iono §6.13.4).
set -u
EXE=./build/x64-Release/gnss_processor_ifg.exe
for name in ifg_off ifg_w ifg_we; do
  mkdir -p "runs/$name/logs"
  t0=$(date +%s)
  echo "=== $name start $(date +%H:%M:%S) ==="
  "$EXE" "config/test_suite/_${name}.json" > "runs/$name/run.log" 2>&1
  echo "exit=$? elapsed=$(( $(date +%s) - t0 ))s"
  bad=""
  for f in network_events.csv network_flag_rates.csv network_solution.csv \
           network_epoch_status.csv network_if_slip.csv network_estimability.csv; do
    src="logs/$f"
    if [ -f "$src" ]; then
      m=$(stat -c %Y "$src" 2>/dev/null || echo 0)
      [ "$m" -ge "$t0" ] || bad="$bad $f(STALE)"
      cp -f "$src" "runs/$name/logs/$f" || bad="$bad $f(copyfail)"
    else bad="$bad $f(nosrc)"; fi
  done
  if [ -n "$bad" ]; then echo "WARN:$bad"; else echo "capture: all fresh"; fi
done

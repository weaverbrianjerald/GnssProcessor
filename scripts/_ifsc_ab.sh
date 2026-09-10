#!/usr/bin/env bash
# SPEC_amb_iono_split_overcount §6.14.5 defect 2 A/B: does excluding the reference
# satellite's structural zero from the IF-TD pool scale change the FLAG population?
#
# The two arms differ only in the EXE (one source hunk, same Release build config).
# Runs are SEQUENTIAL on purpose: concurrent runs share ./logs/ (no config knob for
# the trace dir), so parallel arms would cross-write each other's traces.
# Capture asserts SOURCE mtime >= run start — the stale-trace-dir trap that once
# faked byte-identity by copying a previous run's files.
set -u
for arm in old new; do
  EXE=./build/x64-Release/gnss_processor_ifsc_${arm}.exe
  name=ifsc_${arm}
  mkdir -p "runs/$name/logs"
  t0=$(date +%s)
  echo "=== $name start $(date +%H:%M:%S)  exe=$(basename $EXE) ==="
  "$EXE" "config/test_suite/_${name}.json" > "runs/$name/run.log" 2>&1
  echo "exit=$? elapsed=$(( $(date +%s) - t0 ))s"
  bad=""
  for f in network_flag_rates.csv network_if_slip.csv network_events.csv \
           network_solution.csv network_run_summary.csv network_dd_fractional.csv; do
    src="logs/$f"
    if [ -f "$src" ]; then
      m=$(stat -c %Y "$src" 2>/dev/null || echo 0)
      [ "$m" -ge "$t0" ] || bad="$bad $f(STALE)"
      cp -f "$src" "runs/$name/logs/$f" || bad="$bad $f(copyfail)"
    else bad="$bad $f(nosrc)"; fi
  done
  [ -n "$bad" ] && echo "WARN:$bad"
  echo "captured: $(ls -la runs/$name/logs/ | awk 'NR>3{print $9"="$5}' | tr '\n' ' ')"
done
